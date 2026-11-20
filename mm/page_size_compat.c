// SPDX-License-Identifier: GPL-2.0
/*
 * Page Size Emulation
 *
 * Copyright (c) 2024, Google LLC.
 * Author: Kalesh Singh <kaleshsingh@goole.com>
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kstrtox.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/page_size_compat.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/perf_event.h>

#define MIN_PAGE_SHIFT_COMPAT (PAGE_SHIFT + 1)
#define MAX_PAGE_SHIFT_COMPAT 16 /* Max of 64KB */
#define __MMAP_RND_BITS(x)      (x - (__PAGE_SHIFT - PAGE_SHIFT))

DEFINE_STATIC_KEY_FALSE(page_shift_compat_enabled);
EXPORT_SYMBOL_GPL(page_shift_compat_enabled);

int page_shift_compat __ro_after_init = MIN_PAGE_SHIFT_COMPAT;
EXPORT_SYMBOL_GPL(page_shift_compat);

static int __init early_page_shift_compat(char *buf)
{
	int ret;

	ret = kstrtoint(buf, 10, &page_shift_compat);
	if (ret)
		return ret;

	/* Only supported on 4KB kernel */
	if (PAGE_SHIFT != 12)
		return -ENOTSUPP;

	if (page_shift_compat < MIN_PAGE_SHIFT_COMPAT ||
		page_shift_compat > MAX_PAGE_SHIFT_COMPAT)
		return -EINVAL;

	static_branch_enable(&page_shift_compat_enabled);

	return 0;
}
early_param("page_shift", early_page_shift_compat);

static int __init init_mmap_rnd_bits(void)
{
	if (!static_branch_unlikely(&page_shift_compat_enabled))
		return 0;

#ifdef CONFIG_HAVE_ARCH_MMAP_RND_BITS
	mmap_rnd_bits_min = __MMAP_RND_BITS(CONFIG_ARCH_MMAP_RND_BITS_MIN);
	mmap_rnd_bits_max = __MMAP_RND_BITS(CONFIG_ARCH_MMAP_RND_BITS_MAX);
	mmap_rnd_bits = __MMAP_RND_BITS(CONFIG_ARCH_MMAP_RND_BITS);
#endif

	return 0;
}
core_initcall(init_mmap_rnd_bits);

#ifdef CONFIG_BPF_SYSCALL
bool bpf_is_ringbuf_file(struct file *file)
{
	if (file->f_op != &bpf_map_fops)
		return false;

	struct bpf_map *map = file->private_data;

	return map->map_type == BPF_MAP_TYPE_RINGBUF ||
		map->map_type == BPF_MAP_TYPE_USER_RINGBUF;
}
#else
bool bpf_is_ringbuf_file(struct file *file)
{
	return false;
}
#endif

#define SWAP_MAGIC	"SWAPSPACE2"
#define SWAP_MAGIC_LEN	10

/*
 * Rewrite a swap header laid out in pages of (PAGE_SIZE << shift) in the
 * layout of PAGE_SIZE pages: the magic moves to the end of the first
 * PAGE_SIZE page, and last_page counts PAGE_SIZE pages. The rest of the old
 * header page becomes swap space.
 *
 * Headers that swapon() would refuse anyway are left alone. Bad pages are
 * refused too: nothing on Android writes them, and each one would become
 * several PAGE_SIZE entries, which swapon() can't read while badpages[] is
 * declared with one entry (that traps with CONFIG_UBSAN_BOUNDS).
 */
static int __convert_swap_header(union swap_header *hdr, unsigned int shift,
				 u64 nr_pages)
{
	unsigned int last_page = hdr->info.last_page;

	if (hdr->info.version != 1) {
		pgcompat_err("Unsupported swap header version %u", hdr->info.version);
		return -EINVAL;
	}
	if (!last_page) {
		pgcompat_err("Empty swap area");
		return -EINVAL;
	}
	if (hdr->info.nr_badpages) {
		pgcompat_err("Swap header with bad pages not supported");
		return -EINVAL;
	}
	if (last_page >= U32_MAX >> shift) {
		pgcompat_err("Swap area too large to convert");
		return -EINVAL;
	}
	/* Also refuses a header that a failed conversion left half written */
	if (((u64)last_page + 1) << shift > nr_pages) {
		pgcompat_err("Swap area larger than its file or device");
		return -EINVAL;
	}

	hdr->info.last_page = ((last_page + 1) << shift) - 1;
	memcpy(hdr->magic.magic, SWAP_MAGIC, SWAP_MAGIC_LEN);

	return 0;
}

/*
 * mkswap lays a swap area out in pages of the page size that userspace sees.
 * In the emulated mode, that puts the magic in the last 10 bytes of the first
 * __PAGE_SIZE page, and makes last_page and badpages[] count __PAGE_SIZE
 * pages. swapon() reads the header in the layout of PAGE_SIZE pages, so it
 * would find no magic, or with the magic moved alone, use only a fraction of
 * the area.
 *
 * Convert such a header to the PAGE_SIZE layout and write it back through
 * the filesystem, before swapon() takes the inode lock, so that the
 * filesystem sees an ordinary write. Sync it before swapon() goes on, as
 * swapping and discard can overwrite the old magic. The next swapon() then
 * finds a PAGE_SIZE header, which needs no conversion. A header or file that
 * swapon() refuses is left as it is.
 */
int __fixup_swap_header(struct file *swap_file, struct address_space *mapping)
{
	unsigned int shift = __PAGE_SHIFT - PAGE_SHIFT;
	struct dentry *dentry = swap_file->f_path.dentry;
	struct inode *inode = mapping->host;
	loff_t magic_pos = __PAGE_SIZE - SWAP_MAGIC_LEN;
	char magic[SWAP_MAGIC_LEN];
	union swap_header *hdr;
	struct folio *folio;
	loff_t pos = 0;
	ssize_t written;
	int error = 0;

	/* swapon() fails these itself; leave them as they are */
	if (!shift || !mapping->a_ops->read_folio || IS_SWAPFILE(inode) ||
	    d_unlinked(dentry) || cant_mount(dentry) ||
	    mapping_min_folio_order(mapping) > 0 ||
	    i_size_read(inode) < __PAGE_SIZE)
		return 0;

	folio = read_mapping_folio(mapping, magic_pos >> PAGE_SHIFT, swap_file);
	if (IS_ERR(folio)) {
		pgcompat_err("Failed reading swap magic page");
		return PTR_ERR(folio);
	}
	memcpy_from_folio(magic, folio, offset_in_folio(folio, magic_pos),
			  SWAP_MAGIC_LEN);
	folio_put(folio);

	/* Nothing to do; mkswap tool may have hardcoded a 4096 page size */
	if (memcmp(magic, SWAP_MAGIC, SWAP_MAGIC_LEN))
		return 0;

	hdr = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!hdr)
		return -ENOMEM;

	folio = read_mapping_folio(mapping, 0, swap_file);
	if (IS_ERR(folio)) {
		pgcompat_err("Failed reading swap header page");
		error = PTR_ERR(folio);
		goto out;
	}
	memcpy_from_folio((char *)hdr, folio, 0, PAGE_SIZE);
	folio_put(folio);

	/* A PAGE_SIZE header, such as one converted by an earlier swapon() */
	if (!memcmp(hdr->magic.magic, SWAP_MAGIC, SWAP_MAGIC_LEN))
		goto out;

	error = __convert_swap_header(hdr, shift, i_size_read(inode) >> PAGE_SHIFT);
	if (error)
		goto out;

	/*
	 * RLIMIT_FSIZE would cut the write short, and leave last_page scaled
	 * without the magic that marks the header converted.
	 */
	if (S_ISREG(inode->i_mode) && rlimit(RLIMIT_FSIZE) < PAGE_SIZE) {
		pgcompat_err("RLIMIT_FSIZE too low to convert swap header");
		error = -EFBIG;
		goto out;
	}

	written = kernel_write(swap_file, hdr, PAGE_SIZE, &pos);
	/* A concurrent swapon() of the file won, and swapon() returns -EBUSY */
	if (written == -ETXTBSY)
		goto out;
	if (written != PAGE_SIZE) {
		error = written < 0 ? written : -EIO;
		pgcompat_err("Failed writing swap header: %d", error);
		goto out;
	}
	error = vfs_fsync_range(swap_file, 0, PAGE_SIZE - 1, 1);
	if (error) {
		pgcompat_err("Failed syncing swap header: %d", error);
		goto out;
	}
	pr_info("pgcompat: %pD: converted swap header from %luK to %luK pages\n",
		swap_file, __PAGE_SIZE >> 10, PAGE_SIZE >> 10);
out:
	kfree(hdr);
	return error;
}

#if IS_ENABLED(CONFIG_PERF_EVENTS)
static int __init init_sysctl_perf_event_mlock(void)
{
	if (!static_branch_unlikely(&page_shift_compat_enabled))
		return 0;

	/* Minimum for 512 kiB + 1 user control page */
	sysctl_perf_event_mlock = 512 + (__PAGE_SIZE / 1024); /* 'free' kiB per user */

	return 0;
}
core_initcall(init_sysctl_perf_event_mlock);
#endif
