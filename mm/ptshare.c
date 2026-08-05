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

#include "internal.h"

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
	struct vm_area_struct *vma;
	struct ptshare_desc *desc;
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

	desc = rcu_dereference_protected(mm->ptshare_desc,
					 lockdep_is_held(&mm->mmap_lock));
	if (WARN_ON_ONCE(!desc)) {
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

	ret = ptshare_insert_shadow_vma(vma, desc);
	if (ret)
		goto fallback;

	/* Attach it now that its shadow VMA exists. */
	vma_set_ptshare_desc(vma, desc);
	return 0;

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
