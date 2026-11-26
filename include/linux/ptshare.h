/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PTSHARE_H
#define _LINUX_PTSHARE_H

#ifndef __ASSEMBLY__

#include <linux/mm.h>
#include <linux/refcount.h>
#include <linux/mmu_notifier.h>
#include <linux/android_kabi.h>

struct mm_struct;
struct mem_cgroup;

struct ptshare_desc {
	/* The headless shadow MM for this domain */
	struct mm_struct *ptshare_mm;
	struct mmu_notifier mmu_notifier;
	struct mem_cgroup *memcg;
	refcount_t refcount;
	struct rcu_head rcu;

	ANDROID_BACKPORT_RESERVE(1);
	ANDROID_BACKPORT_RESERVE(2);
};

#ifdef CONFIG_PTSHARE
int __ptshare_unshare_vma(struct vm_area_struct *vma);
int __ptshare_install_vma(struct mm_struct *mm, unsigned long addr,
			  unsigned long len, vm_flags_t vm_flags);
void __ptshare_put_vma(struct vm_area_struct *vma);
void ptshare_exit(struct mm_struct *mm);
void __ptshare_dup_vma(struct mm_struct *mm, struct vm_area_struct *vma);
int __ptshare_unshare_vma_on_gup(struct vm_area_struct *vma, unsigned long addr,
				 unsigned int gup_flags, int *locked);
int __ptshare_unshare_vma_remote(struct mm_struct *mm, unsigned long addr);
unsigned long __ptshare_get_unmapped_area(unsigned long addr, unsigned long len,
					  unsigned long flags, vm_flags_t vm_flags);
int __ptshare_validate_mmap(struct file *file, unsigned long addr,
			    unsigned long len, vm_flags_t *vm_flags);
vm_fault_t ptshare_handle_mm_fault(struct vm_fault *vmf);
bool __ptshare_free_pte_range(struct mmu_gather *tlb, struct ptdesc *pt,
			      unsigned long addr);

/**
 * vma_is_ptshare_client - Check whether a VMA is a page table sharing client
 * @vma: The VMA to check.
 *
 * Use this to skip or refuse operations that must not modify the page
 * tables of @vma, which are shared with the other members of its domain.
 * It stays true until @vma is completely unshared, so it also covers
 * callers that do not hold the mmap lock, such as rmap walks.
 *
 * Return: true if @vma is a client VMA.
 */
static inline bool vma_is_ptshare_client(const struct vm_area_struct *vma)
{
	return vma->vm_flags & VM_PT_SHARED;
}

/**
 * vmg_is_ptshare_client - Check whether a VMA merge maps a client VMA
 * @vmg: The struct vma_merge_struct of the merge.
 *
 * A macro, as struct vma_merge_struct is private to mm/.
 *
 * Return: true if the range being mapped or modified has VM_PT_SHARED.
 */
#define vmg_is_ptshare_client(vmg)	(!!((vmg)->vm_flags & VM_PT_SHARED))

/**
 * vma_shares_pagetables - Check whether a client VMA is attached to its domain
 * @vma: The VMA to check.
 *
 * Use this before acting on the domain of @vma, for example to unshare
 * its page tables. Unlike vma_is_ptshare_client(), it becomes false as
 * soon as @vma drops its reference to the shared page tables, so that
 * the reference is never dropped twice.
 *
 * Return: true if @vma is a client VMA attached to its domain.
 */
static inline bool vma_shares_pagetables(const struct vm_area_struct *vma)
{
	return vma_is_ptshare_client(vma) && READ_ONCE(vma->__vm_ptshare_desc);
}

/**
 * mm_is_ptshare_shadow - Check whether an MM is the shadow MM of a domain
 * @mm: The MM to check.
 *
 * Return: true if @mm owns the shared page tables of a sharing domain.
 */
static inline bool mm_is_ptshare_shadow(const struct mm_struct *mm)
{
	return mm_flags_test(MMF_PTSHARE_SHADOW, mm);
}

/**
 * vma_is_ptshare_shadow - Check whether a VMA belongs to a shadow MM
 * @vma: The VMA to check.
 *
 * A shadow VMA maps the shared page tables directly and keeps them alive
 * for as long as client VMAs use them. Use this to keep operations meant
 * for processes, such as uprobes or THP collapse, away from shadow VMAs.
 *
 * Return: true if @vma is a shadow VMA.
 */
static inline bool vma_is_ptshare_shadow(const struct vm_area_struct *vma)
{
	return vma->vm_mm && mm_is_ptshare_shadow(vma->vm_mm);
}

/**
 * vma_ptshare_desc - Get the domain descriptor of a client VMA
 * @vma: A client VMA.
 *
 * Return: the descriptor, or NULL if @vma is not attached to its domain.
 */
static inline struct ptshare_desc *vma_ptshare_desc(const struct vm_area_struct *vma)
{
	if (WARN_ON_ONCE(!vma_is_ptshare_client(vma)))
		return NULL;

	return READ_ONCE(vma->__vm_ptshare_desc);
}

/**
 * vma_set_ptshare_desc - Attach a client VMA to its domain, or detach it
 * @vma: A client VMA.
 * @desc: The descriptor of the domain, or NULL to detach @vma.
 */
static inline void vma_set_ptshare_desc(struct vm_area_struct *vma,
					struct ptshare_desc *desc)
{
	if (WARN_ON_ONCE(!vma_is_ptshare_client(vma)))
		return;

	WRITE_ONCE(vma->__vm_ptshare_desc, desc);
}

/**
 * vma_ptshare_refcount - Get the client count of a shadow VMA
 * @vma: A shadow VMA.
 *
 * Return: a pointer to the count of client VMAs that use the page tables
 * of @vma, or NULL if @vma is not a shadow VMA.
 */
static inline refcount_t *vma_ptshare_refcount(struct vm_area_struct *vma)
{
	if (WARN_ON_ONCE(!vma_is_ptshare_shadow(vma)))
		return NULL;

	return &vma->__vm_ptshare_refcount;
}

/*
 * vm_area_dup() does not zero the new VMA: a copy of a client VMA keeps
 * its descriptor, every other copy gets a cleared union.
 */
static inline void ptshare_vma_init_from(struct vm_area_struct *dest,
					 const struct vm_area_struct *src)
{
	dest->__vm_ptshare_desc = vma_is_ptshare_client(src) ?
				  READ_ONCE(src->__vm_ptshare_desc) : NULL;
}

/**
 * mm_shares_pagetables - Check whether an MM is attached to a sharing domain
 * @mm: The MM to check.
 *
 * Return: true if @mm is a member of a sharing domain or its shadow MM.
 */
static inline bool mm_shares_pagetables(const struct mm_struct *mm)
{
	return rcu_access_pointer(mm->ptshare_desc);
}

/**
 * ptshare_put_vma - Drop the shadow VMA reference of a client VMA
 * @vma: A VMA that is being freed or unshared.
 *
 * Does nothing if @vma is not a client VMA. When the last client VMA of a
 * shadow VMA drops its reference, the shadow VMA and the page tables it
 * owns are freed.
 *
 * Context: May sleep. Takes the shadow MM mmap_lock, so the caller must
 * not hold an rmap lock.
 */
static inline void ptshare_put_vma(struct vm_area_struct *vma)
{
	if (vma_is_ptshare_client(vma))
		__ptshare_put_vma(vma);
}

/**
 * ptshare_get_unmapped_area - Pick a PMD-aligned address for a client VMA
 * @addr: The caller's hint address, or 0.
 * @len: The length of the mapping.
 * @flags: The MAP_* flags of the request.
 * @vm_flags: The VMA flags of the mapping.
 *
 * Call from __get_unmapped_area() so non-MAP_FIXED client VMA requests get
 * a PMD-aligned address regardless of file offset.
 *
 * Context: The caller holds the mmap_lock of current->mm for write.
 * Return: A PMD-aligned address, 0 to use the default unmapped-area search,
 *         or a negative errno.
 */
static inline unsigned long ptshare_get_unmapped_area(unsigned long addr,
						      unsigned long len,
						      unsigned long flags,
						      vm_flags_t vm_flags)
{
	if (likely(!(vm_flags & VM_PT_SHARED)))
		return 0;

	return __ptshare_get_unmapped_area(addr, len, flags, vm_flags);
}

/**
 * ptshare_validate_mmap - Check a request for a client VMA
 * @file: The file to map, or NULL for an anonymous mapping.
 * @addr: The address of the mapping.
 * @len: The length of the mapping.
 * @vm_flags: The VMA flags of the mapping. VM_PT_SHARED is cleared if
 *            the mapping falls back to private page tables.
 *
 * Call this from do_mmap() right before mmap_region(), so that a request
 * that cannot share page tables fails before anything is unmapped.
 *
 * Context: The caller holds the mmap_lock of current->mm for write.
 * Return: 0, or a negative errno to fail the mmap().
 */
static inline int ptshare_validate_mmap(struct file *file, unsigned long addr,
					unsigned long len,
					vm_flags_t *vm_flags)
{
	if (likely(!(*vm_flags & VM_PT_SHARED)))
		return 0;

	return __ptshare_validate_mmap(file, addr, len, vm_flags);
}

/**
 * ptshare_install_vma - Share the page tables of a new mapping
 * @mm: The mm that mmap_region() mapped the range into.
 * @addr: The start address of the new mapping.
 * @len: The length of the mapping.
 * @vm_flags: The VMA flags that do_mmap() passed to mmap_region().
 *
 * If @vm_flags asks for VM_PT_SHARED, installs the shadow VMA for the new
 * client VMA at @addr, or leaves the VMA with private page tables if that
 * fails. The first client VMA of @mm creates its sharing domain and makes
 * @mm the creator.
 *
 * Context: The caller holds the mmap_lock of @mm for write.
 * Return: 0 if sharing was not requested, cleared by ->mmap(), or the
 * shadow VMA was installed, or a negative errno if installation failed
 * and the VMA fell back to private page tables.
 */
static inline int ptshare_install_vma(struct mm_struct *mm,
				      unsigned long addr, unsigned long len,
				      vm_flags_t vm_flags)
{
	if (vm_flags & VM_PT_SHARED)
		return __ptshare_install_vma(mm, addr, len, vm_flags);

	return 0;
}

/**
 * ptshare_abort_new_vma - Drop VM_PT_SHARED from a VMA that mmap() undoes
 * @vma: The new VMA whose ->mmap() failed, not yet in the VMA tree.
 *
 * Call this before unmap_region() undoes a failed ->mmap(). @vma was never
 * attached to its domain, so page tables that ->mmap() filled are private
 * and must be zapped like those of any other VMA.
 */
static inline void ptshare_abort_new_vma(struct vm_area_struct *vma)
{
	/* VMA is not in the tree yet, so no VMA lock is needed. */
	if (vma_is_ptshare_client(vma))
		__vm_flags_mod(vma, 0, VM_PT_SHARED);
}

/**
 * ptshare_dup_vma - Join a VMA copied by fork() to its sharing domain
 * @mm: The child's mm.
 * @vma: The child's copy of a VMA, already linked into @mm.
 *
 * Takes a shadow VMA reference for a client VMA and attaches @mm to the
 * domain with its first client VMA. Does nothing for other VMAs.
 *
 * Context: The caller holds the mmap_lock of @mm and of the parent's mm
 * for write. May sleep. Takes the shadow MM mmap_lock, so the caller must
 * not hold an rmap lock.
 */
static inline void ptshare_dup_vma(struct mm_struct *mm,
				   struct vm_area_struct *vma)
{
	if (vma_is_ptshare_client(vma))
		__ptshare_dup_vma(mm, vma);
}

/**
 * ptdesc_ptshare_inc - Count a client PMD that points at a shared PTE table
 * @pt: The PTE table, owned by the shadow MM.
 *
 * The count never pins or frees the table; the shadow MM frees it when the
 * last client of the shared mapping unmaps or unshares it, or exits.
 */
static inline void ptdesc_ptshare_inc(struct ptdesc *pt)
{
	atomic_inc(&pt->pt_ptshare_count);
}

/**
 * ptdesc_ptshare_dec - Drop the count of a client PMD, if the table has one
 * @pt: The PTE table.
 *
 * Return: true if the table was shared and the count was dropped.
 */
static inline bool ptdesc_ptshare_dec(struct ptdesc *pt)
{
	return atomic_dec_if_positive(&pt->pt_ptshare_count) >= 0;
}

/**
 * ptdesc_ptshare_count - Number of client PMDs that point at a PTE table
 * @pt: The PTE table.
 *
 * Return: the number of client PMDs that point at @pt.
 */
static inline int ptdesc_ptshare_count(struct ptdesc *pt)
{
	return atomic_read(&pt->pt_ptshare_count);
}

/**
 * ptshare_unshare_vma_locked - Give a client VMA private page tables again
 * @vma: The VMA whose page tables or bounds are about to change.
 *
 * Call this before splitting @vma or changing its PTEs, so that the change
 * does not reach page tables shared with other processes. Does nothing if
 * @vma does not share page tables, so callers that hold the mmap_lock for
 * write can call it for any VMA.
 *
 * Context: The caller holds the mmap_lock of @vma->vm_mm for write and no
 * rmap lock. Unsharing write-locks @vma itself, which needs the mmap write
 * lock and keeps out page faults under the per-VMA lock, and takes the
 * shadow MM's mmap_lock, which nests outside i_mmap_rwsem.
 *
 * Return: 0 if @vma no longer shares page tables, or -EINVAL if it could
 * not be unshared, in which case it is unchanged.
 */
static inline int ptshare_unshare_vma_locked(struct vm_area_struct *vma)
{
	mmap_assert_write_locked(vma->vm_mm);
	if (!vma_shares_pagetables(vma))
		return 0;
	return __ptshare_unshare_vma(vma);
}

/**
 * ptshare_sync_vm_flags - Drop a stale VM_PT_SHARED from precomputed flags
 * @vma: The VMA that @vm_flags are about to be written to.
 * @vm_flags: Flags computed from @vma before it may have been unshared.
 *
 * Unsharing a client VMA, directly or by splitting it, clears VM_PT_SHARED.
 * Callers that compute new flags before such an operation and write them
 * back afterwards pass them through this helper once the operation has
 * returned. Until then the flags must keep VM_PT_SHARED, so that the merge
 * checks keep refusing to merge the VMA while it still shares page tables.
 *
 * Return: @vm_flags, without VM_PT_SHARED if @vma is no longer a client VMA.
 */
static inline vm_flags_t ptshare_sync_vm_flags(const struct vm_area_struct *vma,
					       vm_flags_t vm_flags)
{
	if (!vma_is_ptshare_client(vma))
		vm_flags &= ~VM_PT_SHARED;
	return vm_flags;
}

/**
 * ptshare_unshare_vma_on_gup - Unshare a client VMA before GUP CoWs in it
 * @vma: The VMA that GUP is about to access.
 * @addr: The address that GUP accesses.
 * @gup_flags: The GUP flags of the access.
 * @locked: GUP's lock state, cleared if the mmap_lock was dropped.
 *
 * A FOLL_FORCE write or a read-only long-term pin of a COW mapping would
 * CoW in page tables shared with other processes. Unshare @vma first,
 * with the mmap_lock dropped as after VM_FAULT_RETRY, so that GUP
 * retries on a private VMA.
 *
 * Context: The caller holds the mmap_lock of @vma->vm_mm (for read if
 * @gup_flags has FOLL_UNLOCKABLE without FOLL_NOWAIT).
 * Return:
 * * 0: No unsharing needed. mmap_lock was never dropped, and @vma remains
 *   valid.
 * * -EAGAIN: mmap_lock was dropped (*@locked cleared) either after
 *   unsharing @vma or because a fatal signal interrupted re-locking.
 *   Caller must bail out and retry.
 * * -EFAULT: Unsharing is required, but @gup_flags forbid dropping
 *   mmap_lock. mmap_lock was never dropped, and @vma remains valid.
 * * Other negative errno: Unsharing failed while holding the write lock.
 *   mmap_lock was downgraded back to read (*@locked remains set), but
 *   @vma may be stale.
 */
static inline int ptshare_unshare_vma_on_gup(struct vm_area_struct *vma,
					     unsigned long addr,
					     unsigned int gup_flags,
					     int *locked)
{
	if (likely(!vma_is_ptshare_client(vma)))
		return 0;

	return __ptshare_unshare_vma_on_gup(vma, addr, gup_flags, locked);
}

/**
 * ptshare_get_user_page_vma_remote - Get a page for a remote access
 * @mm: The MM to access.
 * @addr: The address to access.
 * @gup_flags: The GUP flags of the access.
 * @vmap: Returns the VMA that maps @addr.
 *
 * get_user_page_vma_remote() does not let GUP drop the mmap_lock, so GUP
 * cannot unshare a client VMA for a FOLL_FORCE write. Unshare it first.
 *
 * Context: The caller holds the mmap_lock of @mm for read. It may be
 * dropped and retaken.
 * Return: as get_user_page_vma_remote().
 */
static inline struct page *ptshare_get_user_page_vma_remote(struct mm_struct *mm,
							    unsigned long addr,
							    int gup_flags,
							    struct vm_area_struct **vmap)
{
	if ((gup_flags & (FOLL_WRITE | FOLL_FORCE)) == (FOLL_WRITE | FOLL_FORCE) &&
	    unlikely(mm_shares_pagetables(mm))) {
		int err = __ptshare_unshare_vma_remote(mm, addr);

		if (err)
			return ERR_PTR(err);
	}

	return get_user_page_vma_remote(mm, addr, gup_flags, vmap);
}

/**
 * ptshare_free_pte_range - Detach a client PMD from a shared PTE table
 * @tlb: The mmu_gather of the mm that frees its page tables.
 * @pte: The PTE table that free_pte_range() has just unlinked.
 * @addr: The start address of the PMD range that @pte maps.
 *
 * Call this from free_pte_range() after clearing the PMD entry. Any table
 * with a non-zero client PMD count belongs to the shadow MM. In that case,
 * this helper drops the caller's count and queues a TLB flush for the range
 * on @tlb instead of letting the caller free the table.
 *
 * Return: true if @pte is a shared table that the caller must not free,
 *         false otherwise.
 */
static inline bool ptshare_free_pte_range(struct mmu_gather *tlb,
					  pgtable_t pte, unsigned long addr)
{
	/*
	 * A shared table's count still includes this client PMD, so a zero
	 * count means pte is not shared by any client and can be freed.
	 */
	if (likely(!ptdesc_ptshare_count(page_ptdesc(pte))))
		return false;

	return __ptshare_free_pte_range(tlb, page_ptdesc(pte), addr);
}

/* dup_mm() copies the parent's mm_struct: start outside any domain. */
static inline void ptshare_mm_init(struct mm_struct *mm)
{
	RCU_INIT_POINTER(mm->ptshare_desc, NULL);
	refcount_set(&mm->ptshare_vmas, 0);
}

#else /* !CONFIG_PTSHARE */
static inline bool vma_is_ptshare_client(const struct vm_area_struct *vma)
{
	return false;
}

#define vmg_is_ptshare_client(vmg)	((void)(vmg), false)

static inline bool vma_shares_pagetables(const struct vm_area_struct *vma)
{
	return false;
}

static inline bool mm_is_ptshare_shadow(const struct mm_struct *mm)
{
	return false;
}

static inline bool vma_is_ptshare_shadow(const struct vm_area_struct *vma)
{
	return false;
}

static inline struct ptshare_desc *vma_ptshare_desc(const struct vm_area_struct *vma)
{
	return NULL;
}

static inline void vma_set_ptshare_desc(struct vm_area_struct *vma,
					struct ptshare_desc *desc)
{
}

static inline refcount_t *vma_ptshare_refcount(struct vm_area_struct *vma)
{
	return NULL;
}

static inline void ptshare_vma_init_from(struct vm_area_struct *dest,
					 const struct vm_area_struct *src)
{
}

static inline bool mm_shares_pagetables(const struct mm_struct *mm)
{
	return false;
}

static inline void ptshare_put_vma(struct vm_area_struct *vma)
{
}

static inline int ptshare_install_vma(struct mm_struct *mm,
				      unsigned long addr, unsigned long len,
				      vm_flags_t vm_flags)
{
	return 0;
}

static inline void ptshare_abort_new_vma(struct vm_area_struct *vma)
{
}

static inline void ptshare_exit(struct mm_struct *mm)
{
}

static inline void ptshare_dup_vma(struct mm_struct *mm,
				   struct vm_area_struct *vma)
{
}

static inline void ptdesc_ptshare_inc(struct ptdesc *pt)
{
}

static inline bool ptdesc_ptshare_dec(struct ptdesc *pt)
{
	return false;
}

static inline int ptdesc_ptshare_count(struct ptdesc *pt)
{
	return 0;
}

static inline int ptshare_unshare_vma_locked(struct vm_area_struct *vma)
{
	return 0;
}

static inline vm_flags_t ptshare_sync_vm_flags(const struct vm_area_struct *vma,
					       vm_flags_t vm_flags)
{
	return vm_flags;
}

static inline int ptshare_unshare_vma_on_gup(struct vm_area_struct *vma,
					     unsigned long addr,
					     unsigned int gup_flags,
					     int *locked)
{
	return 0;
}

static inline struct page *ptshare_get_user_page_vma_remote(struct mm_struct *mm,
							    unsigned long addr,
							    int gup_flags,
							    struct vm_area_struct **vmap)
{
	return get_user_page_vma_remote(mm, addr, gup_flags, vmap);
}

static inline unsigned long ptshare_get_unmapped_area(unsigned long addr,
						      unsigned long len,
						      unsigned long flags,
						      vm_flags_t vm_flags)
{
	return 0;
}

static inline int ptshare_validate_mmap(struct file *file, unsigned long addr,
					unsigned long len,
					vm_flags_t *vm_flags)
{
	return 0;
}

static inline vm_fault_t ptshare_handle_mm_fault(struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}

static inline bool ptshare_free_pte_range(struct mmu_gather *tlb,
					  pgtable_t pte, unsigned long addr)
{
	return false;
}

static inline void ptshare_mm_init(struct mm_struct *mm)
{
}

#endif /* CONFIG_PTSHARE */

#endif /* !__ASSEMBLY__ */

#endif /* _LINUX_PTSHARE_H */
