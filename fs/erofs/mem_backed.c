// SPDX-License-Identifier: GPL-2.0-only
/*
 * Memory-backed EROFS: mount directly from a kernel memory region by
 * inserting initrd pages into a pseudo-inode's page cache.
 */
#include "internal.h"
#include <linux/pagemap.h>
#include <trace/events/erofs.h>

static unsigned long pending_mem_addr;
static unsigned long pending_mem_size;

void __init erofs_set_mem_region(unsigned long addr, unsigned long size)
{
	pending_mem_addr = addr;
	pending_mem_size = size;
}

bool erofs_has_pending_mem_region(void)
{
	return pending_mem_addr && pending_mem_size;
}

void erofs_consume_pending_mem_region(struct erofs_device_info *dif)
{
	dif->mem_start = pending_mem_addr;
	dif->mem_size = pending_mem_size;
	pending_mem_addr = 0;
	pending_mem_size = 0;
}

/*
 * Page cache population: insert initrd pages into the pseudo inode's
 * mapping.  Boundary pages that already belong to another mapping (e.g.
 * a preceding erofs image) are copied into freshly allocated pages.
 */
int erofs_init_mem_backend(struct super_block *sb)
{
	struct erofs_sb_info *sbi = EROFS_SB(sb);
	unsigned long start = sbi->dif0.mem_start;
	unsigned long size = sbi->dif0.mem_size;
	unsigned long page_start, sub_off;
	pgoff_t nr_pages, index;
	struct inode *inode;
	int err;

	if (!size || start + size < start)
		return -EINVAL;

	page_start = PAGE_ALIGN_DOWN(start);
	sub_off = start - page_start;

	inode = new_inode_pseudo(sb);
	if (!inode)
		return -ENOMEM;

	inode->i_ino = get_next_ino();
	inode->i_mode = S_IRUGO;
	inode->i_flags |= S_PRIVATE;
	simple_inode_init_ts(inode);
	mapping_set_unevictable(inode->i_mapping);

	if (sub_off) {
		/*
		 * Image is not page-aligned: copy into fresh pages so
		 * EROFS block addressing works without a sub-page offset.
		 * Zero-copy is used when the image is page-aligned.
		 */
		pr_info("erofs: mem-backed image not page-aligned (off %lu), copying %lu bytes\n",
			sub_off, size);
		nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
		sbi->dif0.fsoff = 0;
		inode->i_size = size;

		for (index = 0; index < nr_pages; index++) {
			struct page *p = alloc_page(GFP_KERNEL | __GFP_ZERO);
			struct folio *folio;
			unsigned long src = start + index * PAGE_SIZE;
			unsigned long left = size - min_t(unsigned long,
					index * PAGE_SIZE, size);
			unsigned long copy_len = min_t(unsigned long,
					PAGE_SIZE, left);

			if (!p) {
				err = -ENOMEM;
				goto err_undo;
			}
			if (copy_len)
				memcpy(page_address(p), (void *)src, copy_len);

			folio = page_folio(p);
			folio_lock(folio);
			folio->mapping = inode->i_mapping;
			folio->index = index;
			__folio_mark_uptodate(folio);
			err = __filemap_add_folio(inode->i_mapping, folio,
						  index, GFP_KERNEL, NULL);
			folio_unlock(folio);
			if (err) {
				__free_page(p);
				goto err_undo;
			}
		}
	} else {
		nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
		sbi->dif0.fsoff = 0;
		inode->i_size = size;

		for (index = 0; index < nr_pages; index++) {
			unsigned long addr = page_start +
				((unsigned long)index << PAGE_SHIFT);
			struct page *page = virt_to_page(addr);
			struct folio *folio = page_folio(page);
			bool copied = false;

			if (folio->mapping) {
				struct page *copy = alloc_page(GFP_KERNEL);

				if (!copy) {
					err = -ENOMEM;
					goto err_undo;
				}
				memcpy(page_address(copy), page_address(page),
				       PAGE_SIZE);
				page = copy;
				folio = page_folio(copy);
				copied = true;
			} else {
				get_page(page);
			}

			folio_lock(folio);
			folio->mapping = inode->i_mapping;
			folio->index = index;
			__folio_mark_uptodate(folio);
			err = __filemap_add_folio(inode->i_mapping, folio,
						  index, GFP_KERNEL, NULL);
			folio_unlock(folio);
			if (err) {
				if (copied)
					__free_page(page);
				else
					put_page(page);
				goto err_undo;
			}
		}
	}

	sbi->dif0.mem_inode = inode;
	sbi->dif0.mem_start = 0;
	sbi->dif0.mem_size = 0;
	return 0;

err_undo:
	truncate_inode_pages_final(inode->i_mapping);
	iput(inode);
	return err;
}

void erofs_release_mem_backend(struct erofs_sb_info *sbi)
{
	if (sbi->dif0.mem_inode) {
		iput(sbi->dif0.mem_inode);
		sbi->dif0.mem_inode = NULL;
	}
}

/* Uncompressed data read: map extents and copy from pseudo inode page cache. */
static int erofs_mem_scan_folio(struct erofs_map_blocks *map,
				struct inode *inode, struct folio *folio)
{
	struct super_block *sb = inode->i_sb;
	struct erofs_sb_info *sbi = EROFS_SB(sb);
	struct inode *mem_inode = sbi->dif0.mem_inode;
	unsigned int cur = 0, end = folio_size(folio), len;
	loff_t pos = folio_pos(folio), ofs;
	int err = 0;

	erofs_onlinefolio_init(folio);
	while (cur < end) {
		if (!in_range(pos + cur, map->m_la, map->m_llen)) {
			map->m_la = pos + cur;
			map->m_llen = end - cur;
			err = erofs_map_blocks(inode, map);
			if (err)
				break;
		}

		ofs = folio_pos(folio) + cur - map->m_la;
		len = min_t(loff_t, map->m_llen - ofs, end - cur);

		if (map->m_flags & EROFS_MAP_META) {
			struct erofs_buf buf = __EROFS_BUF_INITIALIZER;
			void *src;

			src = erofs_read_metabuf(&buf, sb, map->m_pa + ofs,
						 erofs_inode_in_metabox(inode));
			if (IS_ERR(src)) {
				err = PTR_ERR(src);
				break;
			}
			memcpy_to_folio(folio, cur, src, len);
			erofs_put_metabuf(&buf);
		} else if (!(map->m_flags & EROFS_MAP_MAPPED)) {
			folio_zero_segment(folio, cur, cur + len);
		} else {
			u64 pa = sbi->dif0.fsoff + map->m_pa + ofs;
			pgoff_t mindex = pa >> PAGE_SHIFT;
			size_t moff = offset_in_page(pa);
			size_t left = len;

			while (left) {
				struct folio *src_folio;
				size_t chunk = min(left, PAGE_SIZE - moff);

				src_folio = read_mapping_folio(
					mem_inode->i_mapping, mindex, NULL);
				if (IS_ERR(src_folio)) {
					err = PTR_ERR(src_folio);
					goto out;
				}
				memcpy_to_folio(folio, cur,
						folio_address(src_folio) + moff,
						chunk);
				folio_put(src_folio);

				cur += chunk;
				left -= chunk;
				mindex++;
				moff = 0;
			}
			goto next;
		}
		cur += len;
next:;
	}
out:
	erofs_onlinefolio_end(folio, err, false);
	return err;
}

static int erofs_mem_read_folio(struct file *file, struct folio *folio)
{
	bool need_iput;
	struct inode *realinode =
		erofs_real_inode(folio_inode(folio), &need_iput);
	struct erofs_map_blocks map = {};
	int err;

	trace_erofs_read_folio(realinode, folio, true);
	err = erofs_mem_scan_folio(&map, realinode, folio);
	if (need_iput)
		iput(realinode);
	return err;
}

static void erofs_mem_readahead(struct readahead_control *rac)
{
	bool need_iput;
	struct inode *realinode =
		erofs_real_inode(rac->mapping->host, &need_iput);
	struct erofs_map_blocks map = {};
	struct folio *folio;
	int err;

	trace_erofs_readahead(realinode, readahead_index(rac),
			      readahead_count(rac), true);
	while ((folio = readahead_folio(rac))) {
		err = erofs_mem_scan_folio(&map, realinode, folio);
		if (err && err != -EINTR)
			erofs_err(realinode->i_sb,
				  "readahead error at folio %lu @ nid %llu",
				  folio->index, EROFS_I(realinode)->nid);
	}
	if (need_iput)
		iput(realinode);
}

const struct address_space_operations erofs_mem_aops = {
	.read_folio = erofs_mem_read_folio,
	.readahead = erofs_mem_readahead,
};

/* Bio shim for compressed z_erofs reads from the pseudo inode page cache. */
struct erofs_mem_rq {
	struct bio_vec bvecs[16];
	struct bio bio;
	struct super_block *sb;
};

static void erofs_mem_rq_submit(struct erofs_mem_rq *rq)
{
	struct erofs_sb_info *sbi;
	struct inode *mem_inode;
	struct bio_vec bv;
	struct bvec_iter iter;
	u64 pos;

	if (!rq)
		return;

	sbi = EROFS_SB(rq->sb);
	mem_inode = sbi->dif0.mem_inode;
	pos = (u64)rq->bio.bi_iter.bi_sector << SECTOR_SHIFT;

	bio_for_each_segment(bv, &rq->bio, iter) {
		void *dst = page_address(bv.bv_page) + bv.bv_offset;
		size_t left = bv.bv_len;

		while (left) {
			pgoff_t mindex = pos >> PAGE_SHIFT;
			size_t moff = offset_in_page(pos);
			size_t chunk = min(left, PAGE_SIZE - moff);
			struct folio *src;

			src = read_mapping_folio(mem_inode->i_mapping, mindex,
						 NULL);
			if (IS_ERR(src)) {
				rq->bio.bi_status =
					errno_to_blk_status(PTR_ERR(src));
				goto endio;
			}
			memcpy(dst, folio_address(src) + moff, chunk);
			folio_put(src);

			dst += chunk;
			pos += chunk;
			left -= chunk;
		}
	}
endio:
	bio_endio(&rq->bio);
	bio_uninit(&rq->bio);
	kfree(rq);
}

struct bio *erofs_mem_bio_alloc(struct erofs_map_dev *mdev)
{
	struct erofs_mem_rq *rq =
		kzalloc(sizeof(*rq), GFP_KERNEL | __GFP_NOFAIL);

	bio_init(&rq->bio, NULL, rq->bvecs, ARRAY_SIZE(rq->bvecs), REQ_OP_READ);
	rq->sb = mdev->m_sb;
	return &rq->bio;
}

void erofs_mem_submit_bio(struct bio *bio)
{
	erofs_mem_rq_submit(container_of(bio, struct erofs_mem_rq, bio));
}
