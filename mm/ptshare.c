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
#include <linux/compat.h>
#include <linux/shmem_fs.h>
#include <linux/sizes.h>
#include <linux/hugetlb.h>
#include <linux/mmu_notifier.h>
#include <linux/memcontrol.h>
#include <linux/page_size_compat_defs.h>
#include <linux/mman.h>
#include <linux/pgalloc.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>

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

	/*
	 * handle_pte_fault() can still map PMD-sized file folios via
	 * do_set_pmd() (through filemap_map_pmd() or finish_fault()).
	 * Client PMDs can only link PTE tables, so disable THP on the
	 * shadow MM.
	 */
	mm_flags_set(MMF_DISABLE_THP_COMPLETELY, desc->ptshare_mm);
	ptshare_clear_mm_owner(desc->ptshare_mm);

	/*
	 * All shadow MMs share one static lockdep class so lockdep validates
	 * client -> shadow ordering across fork() and generic mm paths
	 * (which take mmap_lock directly without nested subclasses).
	 */
	lockdep_set_class(&desc->ptshare_mm->mmap_lock,
			  &ptshare_mmap_lock_key);

	refcount_set(&desc->refcount, 1);

	/* Pin the creator's memcg for charging faults on the shadow MM. */
	desc->memcg = get_mem_cgroup_from_mm(current->mm);

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

	mem_cgroup_put(desc->memcg);
	kfree(desc);
}

static bool ptshare_file_supported(struct file *file)
{
	struct inode *inode;

	if (!file || shmem_file(file))
		return false;

	inode = file_inode(file);

	/*
	 * Restrict sharing to regular, non-DAX files on user-mountable
	 * filesystems. Rejecting SB_NOUSER excludes kernel-internal
	 * pseudo-filesystems such as secretmem, dma-buf, and anon inodes
	 * even when they set S_IFREG.
	 */
	return S_ISREG(inode->i_mode) && !IS_DAX(inode) &&
	       !(inode->i_sb->s_flags & SB_NOUSER);
}

static bool ptshare_mmap_is_topdown(unsigned long flags)
{
	/* x86_64 forces bottom-up allocation for 64-bit MAP_32BIT requests. */
	if (!in_compat_syscall() && (flags & MAP_32BIT))
		return false;

	return mm_flags_test(MMF_TOPDOWN, current->mm);
}

static struct mm_struct *ptshare_creator_shadow_mm(struct mm_struct *mm)
{
	struct ptshare_desc *desc;

	if (!mm_flags_test(MMF_PTSHARE_CREATOR, mm))
		return NULL;

	desc = rcu_dereference_protected(mm->ptshare_desc,
					 lockdep_is_held(&mm->mmap_lock));

	return desc ? desc->ptshare_mm : NULL;
}

static bool ptshare_shadow_overlaps(struct mm_struct *ptshare_mm,
				    unsigned long addr, unsigned long len)
{
	bool overlaps;

	mmap_read_lock(ptshare_mm);
	overlaps = !!find_vma_intersection(ptshare_mm, addr, addr + len);
	mmap_read_unlock(ptshare_mm);

	return overlaps;
}

/*
 * Ensure [addr, addr + len) is also unmapped in ptshare_mm. When the range
 * overlaps a shadow VMA, advance past that shadow VMA and search current->mm
 * with vm_unmapped_area(), then re-validate through the arch allocator.
 */
static unsigned long
ptshare_get_unmapped_shadow_area(struct mm_struct *ptshare_mm,
				 unsigned long addr, unsigned long len,
				 unsigned long flags, vm_flags_t vm_flags)
{
	bool compat = in_compat_syscall();
	bool topdown = ptshare_mmap_is_topdown(flags);
	struct vm_unmapped_area_info info = {
		.flags = topdown ? VM_UNMAPPED_AREA_TOPDOWN : 0,
		.length = len,
		.low_limit = PAGE_SIZE,
		.high_limit = arch_get_mmap_end(addr, len, flags),
		.align_mask = PMD_SIZE - 1,
	};
	struct vm_area_struct *shadow_vma;

	mmap_read_lock(ptshare_mm);
	shadow_vma = find_vma_intersection(ptshare_mm, addr, addr + len);
	if (!shadow_vma) {
		mmap_read_unlock(ptshare_mm);
		return addr;
	}

	/*
	 * Match the address bounds of the initial allocation, because the
	 * arch hint path used for re-validation does not enforce them.
	 */
	if (!compat && (flags & MAP_32BIT)) {
		info.low_limit = SZ_1G;
		info.high_limit = SZ_2G;
	} else {
		if (!compat && (flags & MAP_ABOVE4G))
			info.low_limit = SZ_4G;
		if (addr + len > DEFAULT_MAP_WINDOW)
			info.low_limit = max(info.low_limit,
					     DEFAULT_MAP_WINDOW + 1);
		else
			info.high_limit = min(info.high_limit,
					      DEFAULT_MAP_WINDOW);
	}

	do {
		if (topdown)
			info.high_limit = shadow_vma->vm_start;
		else
			info.low_limit = shadow_vma->vm_end;

		if (info.high_limit <= info.low_limit ||
		    info.high_limit - info.low_limit < len) {
			addr = -ENOMEM;
			break;
		}

		/* Find the next PMD-aligned gap in current->mm. */
		addr = vm_unmapped_area(&info);
		if (IS_ERR_VALUE(addr))
			break;

		shadow_vma = find_vma_intersection(ptshare_mm, addr, addr + len);
	} while (shadow_vma);
	mmap_read_unlock(ptshare_mm);

	if (IS_ERR_VALUE(addr))
		return addr;

	/*
	 * Re-validate arch-specific hint constraints such as the compat
	 * limit and 47-bit address boundary.
	 */
	if (mm_get_unmapped_area_vmflags(current->mm, NULL, addr, len,
					 0, flags, vm_flags) != addr)
		return -ENOMEM;

	return addr;
}

unsigned long __ptshare_get_unmapped_area(unsigned long addr, unsigned long len,
					  unsigned long flags, vm_flags_t vm_flags)
{
	struct mm_struct *ptshare_mm = ptshare_creator_shadow_mm(current->mm);
	unsigned long hint = IS_ALIGNED(addr, PMD_SIZE) ? addr : 0;
	unsigned long len_pad = len + PMD_SIZE;
	unsigned long area;

	if ((flags & MAP_FIXED) || !IS_ALIGNED(len, PMD_SIZE))
		return 0;

	if (len_pad < len)
		return -ENOMEM;

	/* Try a PMD-aligned advisory hint with exact len before padding. */
	if (hint && hint <= TASK_SIZE - len &&
	    (!ptshare_mm || !ptshare_shadow_overlaps(ptshare_mm, hint, len))) {
		area = mm_get_unmapped_area_vmflags(current->mm, NULL, hint, len,
						    0, flags, vm_flags);
		if (IS_ERR_VALUE(area) || area == hint)
			return area;
	}

	area = mm_get_unmapped_area_vmflags(current->mm, NULL, 0, len_pad,
					    0, flags, vm_flags);
	if (IS_ERR_VALUE(area))
		return area;

	/*
	 * Place the PMD-aligned range flush at the top of the padded gap for
	 * top-down allocation so consecutive mappings do not leave PMD gaps.
	 */
	if (ptshare_mmap_is_topdown(flags))
		area = round_down(area + PMD_SIZE, PMD_SIZE);
	else
		area = round_up(area, PMD_SIZE);

	if (ptshare_mm)
		area = ptshare_get_unmapped_shadow_area(ptshare_mm, area, len,
							flags, vm_flags);

	return area;
}

int __ptshare_validate_mmap(struct file *file, unsigned long addr,
			    unsigned long len, vm_flags_t *vm_flags)
{
	struct ptshare_desc *desc;

	/* Re-checked on the final vm_file in ptshare_install_vma(). */
	if (unlikely(!ptshare_file_supported(file)))
		return -EINVAL;

	/* Writable mappings are not allowed */
	if (unlikely(*vm_flags & VM_WRITE))
		return -EINVAL;

	/* Require PMD alignment of both base and size */
	if (unlikely(!IS_ALIGNED(addr, PMD_SIZE) ||
		     !IS_ALIGNED(len, PMD_SIZE)))
		return -EINVAL;

	/*
	 * With page size emulation (x86_64 page_shift=), a read fault between
	 * the real PAGE_SIZE-aligned EOF and the emulated __PAGE_SIZE-aligned
	 * EOF maps the zero page (VM_FAULT_NEED_ANONPAGE), and nothing zaps
	 * that PTE when the file grows. In a shared page table, that stale
	 * PTE would be seen by every process in the domain. Fall back to
	 * private page tables here, after the file, flag, and alignment checks
	 * above have already validated the request.
	 */
	if (__PAGE_SIZE != PAGE_SIZE) {
		*vm_flags &= ~VM_PT_SHARED;
		return 0;
	}

	desc = rcu_dereference_protected(current->mm->ptshare_desc,
					 lockdep_is_held(&current->mm->mmap_lock));

	/*
	 * No domain exists yet, so there is no domain creator to verify and no
	 * shadow VMA overlap to check here. ptshare_install_vma() creates the
	 * domain when the VMA is installed.
	 */
	if (!desc)
		return 0;

	/*
	 * Only the mm that created the domain gets MMF_PTSHARE_CREATOR.
	 * Forked children inherit the domain without that flag and cannot
	 * map new client VMAs into it.
	 */
	if (!mm_flags_test(MMF_PTSHARE_CREATOR, current->mm))
		return -EPERM;

	/*
	 * Refuse a request that overlaps an existing shadow VMA so a mapping
	 * cannot replace a range that is already shared in the domain.
	 */
	if (ptshare_shadow_overlaps(desc->ptshare_mm, addr, len))
		return -EINVAL;

	return 0;
}

int __ptshare_unshare_vma(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	struct mmu_notifier_range range;
	unsigned long addr, next;

	if (WARN_ON_ONCE(!IS_ALIGNED(vma->vm_start, PMD_SIZE)) ||
	    WARN_ON_ONCE(!IS_ALIGNED(vma->vm_end, PMD_SIZE)))
		return -EINVAL;

	vma_start_write(vma);

	mmu_notifier_range_init(&range, MMU_NOTIFY_UNMAP, 0, mm,
				vma->vm_start, vma->vm_end);
	mmu_notifier_invalidate_range_start(&range);

	for (addr = vma->vm_start; addr < vma->vm_end; addr = next) {
		pmd_t *pmd = mm_find_pmd(mm, addr);
		spinlock_t *ptl;	/* shared with the shadow MM */
		struct ptdesc *ptdesc;
		pte_t *pte;

		next = pmd_addr_end(addr, vma->vm_end);
		if (!pmd)
			continue;

		/* Lockless walkers recheck the PMD under this lock. */
		pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
		if (!pte)
			continue;
		ptdesc = page_ptdesc(pmd_page(*pmd));
		pmd_clear(pmd);
		mm_dec_nr_ptes(mm);
		pte_unmap_unlock(pte, ptl);
		WARN_ON_ONCE(!ptdesc_ptshare_dec(ptdesc));
	}

	/*
	 * flush_tlb_range() is not leaf-only, so it also drops cached walks
	 * through the cleared PMDs. We don't need mmu_gather here because
	 * the shadow MM frees the tables via RCU.
	 */
	flush_tlb_range(vma, vma->vm_start, vma->vm_end);

	mmu_notifier_invalidate_range_end(&range);

	__ptshare_put_vma(vma);

	/*
	 * Clear the descriptor before clearing VM_PT_SHARED, since
	 * vma_set_ptshare_desc() requires the flag. That also prevents a
	 * double put if stale flags restore VM_PT_SHARED (see
	 * ptshare_sync_vm_flags()).
	 */
	vma_set_ptshare_desc(vma, NULL);
	vm_flags_clear(vma, VM_PT_SHARED);

	return 0;
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

	/*
	 * ->mmap() may replace vm_file (overlayfs, FUSE passthrough, and
	 * fuse-bpf install their backing file) or clear vm_ops, so check the
	 * final VMA again. Rejecting VM_SPECIAL also keeps out special driver
	 * mappings on regular files.
	 */
	if (!ptshare_file_supported(vma->vm_file) || vma_is_anonymous(vma) ||
	    (vma->vm_flags & (VM_SPECIAL | VM_WRITE))) {
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

int __ptshare_unshare_vma_on_gup(struct vm_area_struct *vma, unsigned long addr,
				 unsigned int gup_flags, int *locked)
{
	struct mm_struct *mm = vma->vm_mm;
	int ret;

	mmap_assert_locked(mm);

	/* Only writes and long-term pins CoW, see gup_must_unshare() */
	if (!is_cow_mapping(vma->vm_flags) ||
	    !(gup_flags & (FOLL_WRITE | FOLL_LONGTERM)))
		return 0;
	if ((gup_flags & (FOLL_UNLOCKABLE | FOLL_NOWAIT)) != FOLL_UNLOCKABLE)
		return -EFAULT;

	mmap_read_unlock(mm);
	*locked = 0;

	/* Lock was dropped, so let the caller retry and see the fatal signal. */
	if (mmap_write_lock_killable(mm))
		return -EAGAIN;

	vma = vma_lookup(mm, addr);
	if (!vma)
		goto again;

	ret = ptshare_unshare_vma_locked(vma);
	if (!ret)
		goto again;

	/*
	 * Unsharing failed. GUP expects mmap_lock held for read
	 * (*locked == 1) on non-EAGAIN errors.
	 */
	mmap_write_downgrade(mm);
	*locked = 1;
	return ret;

again:
	/*
	 * Re-locking for write dropped mmap_lock earlier, so the caller's vma
	 * pointer may be stale. Drop mmap_lock here and have the caller retry.
	 */
	mmap_write_unlock(mm);

	return -EAGAIN;
}

int __ptshare_unshare_vma_remote(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *vma = vma_lookup(mm, addr);
	int ret = 0;

	mmap_assert_locked(mm);

	/* Only COW mappings allocate a private copy on a FOLL_FORCE write. */
	if (!vma || !vma_shares_pagetables(vma) || !is_cow_mapping(vma->vm_flags))
		return 0;

	mmap_read_unlock(mm);
	if (mmap_write_lock_killable(mm)) {
		mmap_read_lock(mm);
		return -EINTR;
	}

	vma = vma_lookup(mm, addr);
	if (vma)
		ret = ptshare_unshare_vma_locked(vma);
	mmap_write_downgrade(mm);

	/*
	 * If the VMA vanished while unlocked, return 0 and let
	 * get_user_page_vma_remote() report the missing VMA.
	 */
	return ret;
}

/*
 * Make the creator's memcg active while a fault populates the shadow MM,
 * so page table allocations and page cache reads are charged to the
 * creator. Returns the pinned memcg for ptshare_unbind_active_memcg(),
 * or NULL when memcg is disabled.
 */
static struct mem_cgroup *ptshare_bind_active_memcg(struct ptshare_desc *desc,
						    struct mem_cgroup **old_memcg)
{
	struct mem_cgroup *memcg = desc->memcg;

	if (!memcg)
		return NULL;

	/* Charge the nearest online ancestor if the creator's cgroup is gone. */
	while (!mem_cgroup_tryget_online(memcg))
		memcg = parent_mem_cgroup(memcg);

	*old_memcg = set_active_memcg(memcg);

	return memcg;
}

/*
 * Restore the previous active memcg, even if NULL, before dropping the
 * reference taken by ptshare_bind_active_memcg().
 */
static void ptshare_unbind_active_memcg(struct mem_cgroup *memcg,
					struct mem_cgroup *old_memcg)
{
	struct mem_cgroup *cur;

	if (!memcg)
		return;

	cur = set_active_memcg(old_memcg);
	VM_WARN_ON_ONCE(cur != memcg);
	mem_cgroup_put(memcg);
}

/**
 * ptshare_handle_mm_fault - Fault in a client VMA through the shadow MM
 * @vmf: The fault on the client VMA, with vmf->pmd pointing to the
 *       client PMD entry.
 *
 * Handle the fault on the shadow VMA at the same address, so that the PTE
 * table and its entries are created in the shadow MM, then point an empty
 * client PMD at that table. Only read faults reach here because client
 * VMAs are never writable and GUP unshares them before any CoW fault.
 *
 * Context: Called by __handle_mm_fault() with the client VMA locked as
 * vmf->flags says. Leaves the client fault lock held for the caller.
 * Return: The VM_FAULT_* result of the fault in the shadow MM.
 */
vm_fault_t ptshare_handle_mm_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *ptshare_vma;
	struct mem_cgroup *memcg, *old_memcg = NULL;
	struct ptshare_desc *desc;
	struct mm_struct *ptshare_mm;
	struct ptdesc *ptdesc;
	vm_fault_t ret;
	pgd_t *pgd;
	p4d_t *p4d;

	assert_fault_locked(vmf);

	/*
	 * Unsharing detaches the descriptor under mmap_write_lock and the VMA
	 * write lock, so the client fault lock guarantees desc stays attached.
	 */
	desc = vma_ptshare_desc(vmf->vma);
	if (WARN_ON_ONCE(!desc))
		return VM_FAULT_SIGBUS;

	ptshare_mm = desc->ptshare_mm;

	/*
	 * Write and unshare faults would CoW inside the shared page table.
	 * Client VMAs are never writable and GUP unshares them before it
	 * raises either fault, so neither may reach the shadow MM.
	 */
	if (WARN_ON_ONCE(vmf->flags & (FAULT_FLAG_WRITE | FAULT_FLAG_UNSHARE)))
		return VM_FAULT_SIGSEGV;

	/*
	 * Lock the shadow VMA the way the caller locked the client VMA. A
	 * fault under mmap_lock must not reach handle_pte_fault() with
	 * FAULT_FLAG_VMA_LOCK, because do_read_fault() returns VM_FAULT_RETRY
	 * under a per-VMA lock for files without ->map_pages, so the retry
	 * under mmap_lock would never make progress.
	 */
	if (vmf->flags & FAULT_FLAG_VMA_LOCK) {
		ptshare_vma = lock_vma_under_rcu(ptshare_mm, vmf->address);
		if (!ptshare_vma)
			return VM_FAULT_RETRY;
	} else {
		/* The shadow mmap_lock nests inside the client mmap_lock. */
		mmap_read_lock(ptshare_mm);
		ptshare_vma = vma_lookup(ptshare_mm, vmf->address);
		if (WARN_ON_ONCE(!ptshare_vma)) {
			mmap_read_unlock(ptshare_mm);
			return VM_FAULT_SIGBUS;
		}
	}

	struct vm_fault ptshare_vmf = {
		.vma = ptshare_vma,
		.gfp_mask = vmf->gfp_mask,
		.pgoff = vmf->pgoff,
		.address = vmf->address,
		.real_address = vmf->real_address,
		.flags = vmf->flags,
	};

	assert_fault_locked(&ptshare_vmf);

	/* Every exit below ends this active memcg scope at out. */
	memcg = ptshare_bind_active_memcg(desc, &old_memcg);

	pgd = pgd_offset(ptshare_mm, vmf->address);
	p4d = p4d_alloc(ptshare_mm, pgd, vmf->address);
	if (!p4d) {
		ret = VM_FAULT_OOM;
		goto out;
	}

	ptshare_vmf.pud = pud_alloc(ptshare_mm, p4d, vmf->address);
	if (!ptshare_vmf.pud) {
		ret = VM_FAULT_OOM;
		goto out;
	}

	ptshare_vmf.pmd = pmd_alloc(ptshare_mm, ptshare_vmf.pud, vmf->address);
	if (!ptshare_vmf.pmd) {
		ret = VM_FAULT_OOM;
		goto out;
	}

	ret = handle_pte_fault(&ptshare_vmf);

	/*
	 * Count the client PMD in the shadow PTE table, then point the empty
	 * client PMD at it. If another thread of the client linked the table
	 * first, drop the count again.
	 */
	if (!(ret & (VM_FAULT_ERROR | VM_FAULT_RETRY | VM_FAULT_COMPLETED)) &&
	    pmd_none(pmdp_get_lockless(vmf->pmd))) {
		spinlock_t *shadow_ptl;
		spinlock_t *client_ptl;
		pmd_t pmdval;

		shadow_ptl = pmd_lock(ptshare_mm, ptshare_vmf.pmd);
		pmdval = *ptshare_vmf.pmd;
		if (pmd_present(pmdval)) {
			if (WARN_ON_ONCE(pmd_trans_huge(pmdval))) {
				spin_unlock(shadow_ptl);
				ret = VM_FAULT_SIGBUS;
				goto out;
			}
			ptdesc = page_ptdesc(pmd_page(pmdval));
			ptdesc_ptshare_inc(ptdesc);
		}
		spin_unlock(shadow_ptl);

		if (pmd_present(pmdval)) {
			client_ptl = pmd_lock(vmf->vma->vm_mm, vmf->pmd);
			if (pmd_none(*vmf->pmd)) {
				/*
				 * Order PTE writes before exposing the PMD to
				 * lockless readers.
				 */
				smp_wmb();
				pmd_populate(vmf->vma->vm_mm, vmf->pmd,
					     pmd_pgtable(pmdval));
				mm_inc_nr_ptes(vmf->vma->vm_mm);
			} else {
				/*
				 * Another thread of this client faulted on the
				 * same PMD concurrently. Both threads passed
				 * the lockless pmd_none() check and, to avoid
				 * nesting the two PMD locks, bumped
				 * ptdesc_ptshare_inc() under the shadow PMD
				 * lock before contending on the client PMD
				 * lock. The winner already linked the shadow
				 * PTE table, so undo our speculative increment.
				 * Because the fault lock prevents unmap or
				 * unshare and the shadow VMA never frees its
				 * PTE table while clients exist, *vmf->pmd must
				 * point to this exact same table.
				 */
				WARN_ON_ONCE(!ptdesc_ptshare_dec(ptdesc));
				if (WARN_ON_ONCE(!pmd_present(*vmf->pmd) ||
						 pmd_page(*vmf->pmd) !=
						 pmd_page(pmdval)))
					ret = VM_FAULT_SIGBUS;
			}
			spin_unlock(client_ptl);
		}
	}

out:
	ptshare_unbind_active_memcg(memcg, old_memcg);

	/*
	 * handle_pte_fault() already released the shadow fault lock when
	 * fault_drops_lock() is true. Otherwise release it here. The client
	 * fault lock stays held for the caller in both cases.
	 */
	if (!fault_drops_lock(ret, vmf->flags))
		release_fault_lock(&ptshare_vmf);

	return ret;
}

bool __ptshare_free_pte_range(struct mmu_gather *tlb, struct ptdesc *pt,
			      unsigned long addr)
{
	if (!ptdesc_ptshare_dec(pt))
		return false;

	/*
	 * The zap of the client VMA was skipped, so nothing has flushed the
	 * range yet. Queue a freed-table flush to invalidate both leaf and
	 * walk-cache entries on the client. tlb_finish_mmu() issues the flush
	 * before remove_vma() drops the shadow VMA reference.
	 */
	tlb_flush_pte_range(tlb, addr, PMD_SIZE);
	tlb->freed_tables = 1;
	mm_dec_nr_ptes(tlb->mm);

	return true;
}
