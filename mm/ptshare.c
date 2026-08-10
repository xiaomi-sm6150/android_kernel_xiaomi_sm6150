// SPDX-License-Identifier: GPL-2.0
/*
 * Shared Page Table Support
 *
 * The ptshare infrastructure lets processes share PTE-level page
 * tables for file-backed mappings that are mapped without write
 * permission. Paths that can still write through them, such as
 * mprotect() or FOLL_FORCE writes, unshare the tables first.
 * This is achieved by using a headless shadow MM (ptshare_mm) to
 * host the shared page tables.
 *
 * Copyright (c) 2026, Google LLC.
 * Author: Kalesh Singh <kaleshsingh@google.com>
 */
#include <linux/ptshare.h>
#include <linux/file.h>
#include <linux/mmap_lock.h>
#include <linux/mempolicy.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>

#include "internal.h"

static struct lock_class_key ptshare_mmap_lock_key;

/* No task owns a shadow MM, so don't leave it pointing at its creator. */
static void ptshare_clear_mm_owner(struct mm_struct *mm)
{
#ifdef CONFIG_MEMCG
	RCU_INIT_POINTER(mm->owner, NULL);
#endif
}

static struct ptshare_desc *ptshare_alloc_desc(void)
{
	struct ptshare_desc *desc;

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return NULL;

	desc->ptshare_mm = mm_alloc();
	if (!desc->ptshare_mm) {
		kfree(desc);
		return NULL;
	}
	mm_flags_set(MMF_PTSHARE_SHADOW, desc->ptshare_mm);
	ptshare_clear_mm_owner(desc->ptshare_mm);

	/*
	 * All shadow MMs share one static lockdep class so lockdep validates
	 * client -> shadow ordering across fork() and generic mm paths
	 * (which take mmap_lock directly without nested subclasses).
	 */
	lockdep_set_class(&desc->ptshare_mm->mmap_lock,
			  &ptshare_mmap_lock_key);

	refcount_set(&desc->refcount, 1);

	/* ptshare_exit() of the shadow MM frees the descriptor. */
	rcu_assign_pointer(desc->ptshare_mm->ptshare_desc, desc);

	return desc;
}

/*
 * The last reference may be dropped with a member's mmap_lock held, so
 * leave the teardown of the shadow MM to a workqueue.
 */
static void ptshare_put_desc(struct ptshare_desc *desc)
{
	if (refcount_dec_and_test(&desc->refcount))
		mmput_async(desc->ptshare_mm);
}

static void ptshare_get_desc(struct ptshare_desc *desc)
{
	refcount_inc(&desc->refcount);
}

static void ptshare_desc_rcu_free(struct rcu_head *head)
{
	struct ptshare_desc *desc = container_of(head, struct ptshare_desc, rcu);

	kfree(desc);
}

int __ptshare_unshare_vma(struct vm_area_struct *vma)
{
	return -EINVAL;
}

/* Copy a new client VMA into the shadow MM, with one client reference. */
static struct vm_area_struct *ptshare_alloc_shadow_vma(struct vm_area_struct *vma,
						       struct mm_struct *ptshare_mm)
{
	struct vm_area_struct *ptshare_vma = vm_area_dup(vma);

	if (!ptshare_vma)
		return NULL;

	ptshare_vma->vm_mm = ptshare_mm;

	/* VM_PT_SHARED marks client VMAs only. */
	vm_flags_init(ptshare_vma, vma->vm_flags & ~VM_PT_SHARED);
	refcount_set(vma_ptshare_refcount(ptshare_vma), 1);

	return ptshare_vma;
}

static int ptshare_insert_shadow_vma(struct vm_area_struct *vma,
				     struct ptshare_desc *desc)
{
	struct mm_struct *ptshare_mm = desc->ptshare_mm;
	struct vm_area_struct *ptshare_vma;
	int ret = 0;

	mmap_assert_write_locked(vma->vm_mm);
	mmap_write_lock(ptshare_mm);

	ptshare_vma = ptshare_alloc_shadow_vma(vma, ptshare_mm);
	if (!ptshare_vma) {
		ret = -ENOMEM;
		goto unlock;
	}

	ret = vma_dup_policy(vma, ptshare_vma);
	if (ret)
		goto free_vma;

	if (ptshare_vma->vm_file)
		get_file(ptshare_vma->vm_file);

	if (ptshare_vma->vm_ops && ptshare_vma->vm_ops->open)
		ptshare_vma->vm_ops->open(ptshare_vma);

	ret = insert_vm_struct(ptshare_mm, ptshare_vma);
	if (ret)
		goto put_file;

	/* Unmapping the shadow VMA subtracts these again. */
	vm_stat_account(ptshare_mm, ptshare_vma->vm_flags, vma_pages(ptshare_vma));
	goto unlock;

put_file:
	vma_close(ptshare_vma);

	if (ptshare_vma->vm_file)
		fput(ptshare_vma->vm_file);

	mpol_put(vma_policy(ptshare_vma));

free_vma:
	vm_area_free(ptshare_vma);

unlock:
	mmap_write_unlock(ptshare_mm);

	return ret;
}

/* Return true if any PMD entry in [addr, end) is populated. */
static bool ptshare_range_has_pgtables(struct mm_struct *mm,
				       unsigned long addr, unsigned long end)
{
	unsigned long next;

	mmap_assert_write_locked(mm);

	for (; addr < end; addr = next) {
		pmd_t *pmd = mm_find_pmd(mm, addr);

		next = pmd_addr_end(addr, end);
		if (pmd && !pmd_none(pmdp_get(pmd)))
			return true;
	}

	return false;
}

int __ptshare_install_vma(struct mm_struct *mm, unsigned long addr,
			  unsigned long len, vm_flags_t vm_flags)
{
	struct ptshare_desc *desc, *new_desc = NULL;
	struct vm_area_struct *vma;
	int ret;

	mmap_assert_write_locked(mm);

	vma = vma_lookup(mm, addr);

	/* mmap_region() has just mapped it, with the mmap_lock still held. */
	if (WARN_ON_ONCE(!vma))
		return -ENOENT;

	/* Nothing to do if an ->mmap() hook cleared VM_PT_SHARED. */
	if (!vma_is_ptshare_client(vma))
		return 0;

	/* Enforce remaining invariants on the installed VMA */
	if (!vma->vm_file || vma_is_anonymous(vma) ||
	    (vma->vm_flags & VM_SPECIAL)) {
		ret = -EINVAL;
		goto fallback;
	}

	/*
	 * Client PMDs must be empty or point at tables owned by the shadow MM.
	 * Don't share a range that already has page tables, e.g. populated by
	 * a vendor hook in mmap_region().
	 */
	if (ptshare_range_has_pgtables(mm, addr, addr + len)) {
		ret = -EBUSY;
		goto fallback;
	}

	desc = rcu_dereference_protected(mm->ptshare_desc,
					 lockdep_is_held(&mm->mmap_lock));
	if (!desc) {
		new_desc = ptshare_alloc_desc();
		if (!new_desc) {
			ret = -ENOMEM;
			goto fallback;
		}
		desc = new_desc;
	}

	ret = ptshare_insert_shadow_vma(vma, desc);
	if (ret)
		goto put_desc;

	/* The first client VMA creates the domain and makes @mm its creator. */
	if (new_desc) {
		rcu_assign_pointer(mm->ptshare_desc, new_desc);
		mm_flags_set(MMF_PTSHARE_CREATOR, mm);
	}

	/* Attach it now that its shadow VMA exists. */
	vma_set_ptshare_desc(vma, desc);
	return 0;

put_desc:
	if (new_desc)
		ptshare_put_desc(new_desc);
fallback:
	/* Leave it an ordinary mapping with private page tables. */
	vm_flags_clear(vma, VM_PT_SHARED);

	/*
	 * vma_supports_mlock() returns false for client VMAs, so
	 * __mmap_complete() cleared VM_LOCKED_MASK while VM_PT_SHARED was
	 * set. Restore it now that this VMA has private page tables, so
	 * do_mmap() will populate and mlock it as requested.
	 */
	if ((vm_flags & VM_LOCKED) && vma_supports_mlock(vma)) {
		vm_flags_set(vma, vm_flags & VM_LOCKED_MASK);
		mm->locked_vm += vma_pages(vma);
	}

	return ret;
}

static void ptshare_remove_shadow_vma(struct ptshare_desc *desc,
				      unsigned long start, unsigned long end)
{
	struct mm_struct *ptshare_mm = desc->ptshare_mm;
	struct vm_area_struct *vma;
	struct vma_iterator vmi;
	int err = -EINVAL;

	vma_iter_init(&vmi, ptshare_mm, start);

	/*
	 * This can run in any task's context (remote GUP unshare, final
	 * mmput() by another task), so avoid do_munmap() because it checks
	 * the range against current's TASK_SIZE, which can be smaller than
	 * the shadow address for compat tasks. The shadow VMA is never
	 * split or merged, so it covers [start, end) exactly.
	 */
	mmap_write_lock(ptshare_mm);
	vma = vma_find(&vmi, end);
	if (vma && vma->vm_start == start && vma->vm_end == end)
		err = do_vmi_align_munmap(&vmi, vma, ptshare_mm, start, end,
					  NULL, false);
	WARN_ON_ONCE(err);
	mmap_write_unlock(ptshare_mm);
}

static int ptshare_get_vma(struct vm_area_struct *vma, struct ptshare_desc *desc)
{
	unsigned long addr = vma->vm_start;
	struct vm_area_struct *shadow_vma;

	mmap_read_lock(desc->ptshare_mm);
	shadow_vma = vma_lookup(desc->ptshare_mm, addr);
	if (WARN_ON_ONCE(!shadow_vma)) {
		mmap_read_unlock(desc->ptshare_mm);
		return -EINVAL;
	}
	refcount_inc(vma_ptshare_refcount(shadow_vma));
	mmap_read_unlock(desc->ptshare_mm);

	return 0;
}

void __ptshare_put_vma(struct vm_area_struct *vma)
{
	struct ptshare_desc *desc = vma_ptshare_desc(vma);
	unsigned long addr = vma->vm_start;
	struct vm_area_struct *shadow_vma;
	bool should_remove = false;
	unsigned long start, end;

	if (!desc)
		return;

	mmap_read_lock(desc->ptshare_mm);
	shadow_vma = vma_lookup(desc->ptshare_mm, addr);
	if (WARN_ON_ONCE(!shadow_vma)) {
		mmap_read_unlock(desc->ptshare_mm);
		return;
	}

	start = shadow_vma->vm_start;
	end = shadow_vma->vm_end;

	if (refcount_dec_and_test(vma_ptshare_refcount(shadow_vma)))
		should_remove = true;

	mmap_read_unlock(desc->ptshare_mm);

	if (should_remove)
		ptshare_remove_shadow_vma(desc, start, end);
}

/**
 * ptshare_exit - Leave the sharing domain of an exiting mm
 * @mm: The mm that __mmput() tears down.
 *
 * Call after exit_mmap(), since removing the client VMAs of @mm still needs
 * the domain. When @mm is a shadow MM, this frees the descriptor of its
 * domain.
 */
void ptshare_exit(struct mm_struct *mm)
{
	struct ptshare_desc *desc = rcu_dereference_raw(mm->ptshare_desc);

	if (desc) {
		RCU_INIT_POINTER(mm->ptshare_desc, NULL);
		if (mm_is_ptshare_shadow(mm))
			call_rcu(&desc->rcu, ptshare_desc_rcu_free);
		else
			ptshare_put_desc(desc);
	}
}

void __ptshare_dup_vma(struct mm_struct *mm, struct vm_area_struct *vma)
{
	struct ptshare_desc *desc = vma_ptshare_desc(vma);

	/*
	 * The parent's VMA holds its reference, so this can't fail. If it
	 * does, the copy, which has no page tables yet, faults privately.
	 */
	if (WARN_ON_ONCE(!desc) || ptshare_get_vma(vma, desc)) {
		vma_set_ptshare_desc(vma, NULL);
		vm_flags_clear(vma, VM_PT_SHARED);
		return;
	}

	if (!mm_shares_pagetables(mm)) {
		rcu_assign_pointer(mm->ptshare_desc, desc);
		ptshare_get_desc(desc);
	}
}
