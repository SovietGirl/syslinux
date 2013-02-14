/* ----------------------------------------------------------------------- *
 *
 *   Copyright 2003-2009 H. Peter Anvin - All Rights Reserved
 *   Copyright 2009-2010 Intel Corporation; author: H. Peter Anvin
 *   Copyright 2010 Shao Miller
 *   Copyright 2010-2012 Michal Soltys
 *
 *   Permission is hereby granted, free of charge, to any person
 *   obtaining a copy of this software and associated documentation
 *   files (the "Software"), to deal in the Software without
 *   restriction, including without limitation the rights to use,
 *   copy, modify, merge, publish, distribute, sublicense, and/or
 *   sell copies of the Software, and to permit persons to whom
 *   the Software is furnished to do so, subject to the following
 *   conditions:
 *
 *   The above copyright notice and this permission notice shall
 *   be included in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 *   OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 *   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 *   OTHER DEALINGS IN THE SOFTWARE.
 *
 * ----------------------------------------------------------------------- */

/*
 * partiter.c
 *
 * Provides disk / partition iteration.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <zlib.h>
#include <syslinux/disk.h>
#include "partiter.h"
#include "utility.h"

#define ost_is_ext(type) ((type) == 0x05 || (type) == 0x0F || (type) == 0x85)
#define ost_is_nondata(type) (ost_is_ext(type) || (type) == 0x00)
#define sane(s,l) ((s)+(l) > (s))

/* virtual forwards */

static void pi_dtor_(struct part_iter *);
static int  pi_next_(struct part_iter *);
static int  pi_dos_next(struct part_iter *);
static int  pi_gpt_next(struct part_iter *);

/* vtab and types */

static struct itertype types[] = {
   [0] = {
	.dtor = &pi_dtor_,
	.next = &pi_dos_next,
}, [1] = {
	.dtor = &pi_dtor_,
	.next = &pi_gpt_next,
}, [2] = {
	.dtor = &pi_dtor_,
	.next = &pi_next_,
}};

const struct itertype * const typedos = types;
const struct itertype * const typegpt = types+1;
const struct itertype * const typeraw = types+2;

/* pi_dtor_() - common/raw iterator cleanup */
static void pi_dtor_(struct part_iter *iter)
{
    /* syslinux's free is null resilient */
    free(iter->data);
}

/* pi_ctor() - common/raw iterator initialization */
static int pi_ctor(struct part_iter *iter,
	const struct disk_info *di, int flags
)
{
    memcpy(&iter->di, di, sizeof(struct disk_info));
    iter->flags = flags;
    iter->index0 = -1;
    iter->length = di->lbacnt;

    iter->type = typeraw;
    return 0;
}

/* pi_dos_ctor() - MBR/EBR iterator specific initialization */
static int pi_dos_ctor(struct part_iter *iter,
	const struct disk_info *di, int flags,
	const struct disk_dos_mbr *mbr
)
{
    if (pi_ctor(iter, di, flags))
	return -1;

    if (!(iter->data = malloc(sizeof(struct disk_dos_mbr)))) {
	critm();
	goto bail;
    }

    memcpy(iter->data, mbr, sizeof(struct disk_dos_mbr));

    iter->dos.bebr_index0 = -1;
    iter->dos.disk_sig = mbr->disk_sig;

    iter->type = typedos;
    return 0;
bail:
    pi_dtor_(iter);
    return -1;
}

/* pi_gpt_ctor() - GPT iterator specific initialization */
static int pi_gpt_ctor(struct part_iter *iter,
	const struct disk_info *di, int flags,
	const struct disk_gpt_header *gpth, const struct disk_gpt_part_entry *gptl
)
{
    uint64_t siz;

    if (pi_ctor(iter, di, flags))
	return -1;

    siz = (uint64_t)gpth->part_count * gpth->part_size;

    if (!(iter->data = malloc((size_t)siz))) {
	critm();
	goto bail;
    }

    memcpy(iter->data, gptl, (size_t)siz);

    iter->gpt.pe_count = (int)gpth->part_count;
    iter->gpt.pe_size = (int)gpth->part_size;
    iter->gpt.ufirst = gpth->lba_first_usable;
    iter->gpt.ulast = gpth->lba_last_usable;

    memcpy(&iter->gpt.disk_guid, &gpth->disk_guid, sizeof(struct guid));
    memcpy(&iter->gpt.part_guid, &gpth->disk_guid, sizeof(struct guid));

    iter->type = typegpt;
    return 0;
bail:
    pi_dtor_(iter);
    return -1;
}

/* Logical partition must be sane, meaning:
 * - must be data or empty
 * - must have non-0 start and length
 * - values must not wrap around 32bit
 * - must be inside current EBR frame
 */

static int notsane_logical(const struct part_iter *iter)
{
    const struct disk_dos_part_entry *dp;
    uint32_t end_log;

    dp = ((struct disk_dos_mbr *)iter->data)->table;

    if (!dp[0].ostype)
	return 0;

    if (ost_is_ext(dp[0].ostype)) {
	error("The 1st EBR entry must be data or empty.");
	return -1;
    }

    if (iter->flags & PIF_RELAX)
	return 0;

    end_log = dp[0].start_lba + dp[0].length;

    if (!dp[0].start_lba ||
	!dp[0].length ||
	!sane(dp[0].start_lba, dp[0].length) ||
	end_log > iter->dos.nebr_siz) {

	error("Logical partition (in EBR) with invalid offset and/or length.");
	return -1;
    }

    return 0;
}

/* Extended partition must be sane, meaning:
 * - must be extended or empty
 * - must have non-0 start and length
 * - values must not wrap around 32bit
 * - must be inside base EBR frame
 */

static int notsane_extended(const struct part_iter *iter)
{
    const struct disk_dos_part_entry *dp;
    uint32_t end_ebr;

    dp = ((struct disk_dos_mbr *)iter->data)->table;

    if (!dp[1].ostype)
	return 0;

    if (!ost_is_nondata(dp[1].ostype)) {
	error("The 2nd EBR entry must be extended or empty.");
	return -1;
    }

    if (iter->flags & PIF_RELAX)
	return 0;

    end_ebr = dp[1].start_lba + dp[1].length;

    if (!dp[1].start_lba ||
	!dp[1].length ||
	!sane(dp[1].start_lba, dp[1].length) ||
	end_ebr > iter->dos.bebr_siz) {

	error("Extended partition (EBR) with invalid offset and/or length.");
	return -1;
    }

    return 0;
}

/* Primary partition must be sane, meaning:
 * - must have non-0 start and length
 * - values must not wrap around 32bit
 */

static int notsane_primary(const struct part_iter *iter)
{
    const struct disk_dos_part_entry *dp;
    dp = ((struct disk_dos_mbr *)iter->data)->table + iter->index0;

    if (!dp->ostype)
	return 0;

    if (iter->flags & PIF_RELAX)
	return 0;

    if (!dp->start_lba ||
	!dp->length ||
	!sane(dp->start_lba, dp->length) ||
	dp->start_lba + dp->length > iter->di.lbacnt) {
	error("Primary partition (in MBR) with invalid offset and/or length.");
	return -1;
    }

    return 0;
}

static int notsane_gpt(const struct part_iter *iter)
{
    const struct disk_gpt_part_entry *gp;
    gp = (const struct disk_gpt_part_entry *)
	(iter->data + iter->index0 * iter->gpt.pe_size);

    if (guid_is0(&gp->type))
	return 0;

    if (iter->flags & PIF_RELAX)
	return 0;

    if (gp->lba_first < iter->gpt.ufirst ||
	gp->lba_last > iter->gpt.ulast) {
	error("LBA sectors of GPT partition are beyond the range allowed in GPT header.");
	return -1;
    }

    return 0;
}

static int dos_next_mbr(struct part_iter *iter, uint32_t *lba,
			    struct disk_dos_part_entry **_dp)
{
    struct disk_dos_part_entry *dp;

    while (++iter->index0 < 4) {
	dp = ((struct disk_dos_mbr *)iter->data)->table + iter->index0;

	if (notsane_primary(iter)) {
	    iter->status = PI_INSANE;
	    return -1;
	}

	if (ost_is_ext(dp->ostype)) {
	    if (iter->dos.bebr_index0 >= 0) {
		error("More than 1 extended partition.");
		iter->status = PI_INSANE;
		return -1;
	    }
	    /* record base EBR index */
	    iter->dos.bebr_index0 = iter->index0;
	}
	if (!ost_is_nondata(dp->ostype) || (iter->flags & PIF_STEPALL)) {
	    *lba = dp->start_lba;
	    *_dp = dp;
	    break;
	}
    }

    return 0;
}

static int prep_base_ebr(struct part_iter *iter)
{
    struct disk_dos_part_entry *dp;

    if (iter->dos.bebr_index0 < 0)	/* if we don't have base extended partition at all */
	return -1;
    else if (!iter->dos.bebr_lba) { /* if not initialized yet */
	dp = ((struct disk_dos_mbr *)iter->data)->table + iter->dos.bebr_index0;

	iter->dos.bebr_lba = dp->start_lba;
	iter->dos.bebr_siz = dp->length;

	iter->dos.nebr_lba = dp->start_lba;
	iter->dos.nebr_siz = dp->length;

	iter->index0--;
    }
    return 0;
}

static int dos_next_ebr(struct part_iter *iter, uint32_t *lba,
			    struct disk_dos_part_entry **_dp)
{
    struct disk_dos_part_entry *dp;

    if (prep_base_ebr(iter) < 0) {
	iter->status = PI_DONE;
	return -1;
    }

    while (++iter->index0 < 1024 && iter->dos.nebr_lba) {
	free(iter->data);
	if (!(iter->data =
		    disk_read_sectors(&iter->di, iter->dos.nebr_lba, 1))) {
	    error("Couldn't load EBR.");
	    iter->status = PI_ERRLOAD;
	    return -1;
	}

	/* check sanity of loaded data */
	if (notsane_logical(iter) || notsane_extended(iter)) {
	    iter->status = PI_INSANE;
	    return -1;
	}

	dp = ((struct disk_dos_mbr *)iter->data)->table;

	iter->dos.cebr_lba = iter->dos.nebr_lba;

	/* setup next frame values */
	if (dp[1].ostype) {
	    iter->dos.nebr_lba = iter->dos.bebr_lba + dp[1].start_lba;
	    iter->dos.nebr_siz = dp[1].length;
	} else {
	    iter->dos.nebr_lba = 0;
	    iter->dos.nebr_siz = 0;
	}

	if (!dp[0].ostype)
	    iter->dos.logskipcnt++;

	if (dp[0].ostype || (iter->flags & PIF_STEPALL)) {
	    *lba = iter->dos.cebr_lba + dp[0].start_lba;
	    *_dp = dp;
	    return 0;
	}
	/*
	 * This way it's possible to continue, if some crazy soft left a "hole"
	 * - EBR with a valid extended partition without a logical one. In
	 * such case, linux will not reserve a number for such hole - so we
	 * don't increase index0. If PIF_STEPALL flag is set, we will never
	 * reach this place.
	 */
    }
    iter->status = PI_DONE;
    return -1;
}

static void gpt_conv_label(struct part_iter *iter)
{
    const struct disk_gpt_part_entry *gp;
    const int16_t *orig_lab;

    gp = (const struct disk_gpt_part_entry *)
	(iter->data + iter->index0 * iter->gpt.pe_size);
    orig_lab = (const int16_t *)gp->name;

    /* caveat: this is very crude conversion */
    for (int i = 0; i < PI_GPTLABSIZE/2; i++) {
	iter->gpt.part_label[i] = (char)orig_lab[i];
    }
    iter->gpt.part_label[PI_GPTLABSIZE/2] = 0;
}

static inline int valid_crc(uint32_t crc, const uint8_t *buf, unsigned int siz)
{
    return crc == crc32(crc32(0, NULL, 0), buf, siz);
}

static int gpt_check_hdr_crc(const struct disk_info * const diskinfo, struct disk_gpt_header **_gh)
{
    struct disk_gpt_header *gh = *_gh;
    uint64_t lba_alt;
    uint32_t hold_crc32;

    hold_crc32 = gh->chksum;
    gh->chksum = 0;
    if (!valid_crc(hold_crc32, (const uint8_t *)gh, gh->hdr_size)) {
	warn("Primary GPT header checksum invalid.");
	/* retry with backup */
	lba_alt = gh->lba_alt;
	free(gh);
	if (!(gh = *_gh = disk_read_sectors(diskinfo, lba_alt, 1))) {
	    error("Couldn't read backup GPT header.");
	    return -1;
	}
	hold_crc32 = gh->chksum;
	gh->chksum = 0;
	if (!valid_crc(hold_crc32, (const uint8_t *)gh, gh->hdr_size)) {
	    error("Secondary GPT header checksum invalid.");
	    return -1;
	}
    }
    /* restore old checksum */
    gh->chksum = hold_crc32;

    return 0;
}

static int pi_next_(struct part_iter *iter)
{
    iter->status = PI_DONE;
    return iter->status;
}

static int pi_dos_next(struct part_iter *iter)
{
    uint32_t start_lba = 0;
    struct disk_dos_part_entry *dos_part = NULL;

    if (iter->status)
	return iter->status;

    /* look for primary partitions */
    if (iter->index0 < 4 &&
	    dos_next_mbr(iter, &start_lba, &dos_part) < 0)
	return iter->status;

    /* look for logical partitions */
    if (iter->index0 >= 4 &&
	    dos_next_ebr(iter, &start_lba, &dos_part) < 0)
	return iter->status;

    /*
     * note special index handling:
     * in case PIF_STEPALL is set - this makes the index consistent with
     * non-PIF_STEPALL iterators
     */

    if (!dos_part->ostype)
	iter->index = -1;
    else
	iter->index = iter->index0 + 1 - iter->dos.logskipcnt;
    iter->start_lba = start_lba;
    iter->length = dos_part->length;
    iter->record = (char *)dos_part;

#ifdef DEBUG
    disk_dos_part_dump(dos_part);
#endif

    return iter->status;
}

static int pi_gpt_next(struct part_iter *iter)
{
    const struct disk_gpt_part_entry *gpt_part = NULL;

    if (iter->status)
	return iter->status;

    while (++iter->index0 < iter->gpt.pe_count) {
	gpt_part = (const struct disk_gpt_part_entry *)
	    (iter->data + iter->index0 * iter->gpt.pe_size);

	if (notsane_gpt(iter)) {
	    iter->status = PI_INSANE;
	    return iter->status;
	}

	if (!guid_is0(&gpt_part->type) || (iter->flags & PIF_STEPALL))
	    break;
    }
    /* no more partitions ? */
    if (iter->index0 == iter->gpt.pe_count) {
	iter->status = PI_DONE;
	return iter->status;
    }
    /* gpt_part is guaranteed to be valid here */
    iter->index = iter->index0 + 1;
    iter->start_lba = gpt_part->lba_first;
    iter->length = gpt_part->lba_last - gpt_part->lba_first + 1;
    iter->record = (char *)gpt_part;
    memcpy(&iter->gpt.part_guid, &gpt_part->uid, sizeof(struct guid));
    gpt_conv_label(iter);

#ifdef DEBUG
    disk_gpt_part_dump(gpt_part);
#endif

    return iter->status;
}

static struct part_iter *pi_alloc(void)
{
    struct part_iter *iter;
    if (!(iter = malloc(sizeof(struct part_iter))))
	critm();
    else
	memset(iter, 0, sizeof(struct part_iter));
    return iter;
}

/* pi_del() - delete iterator */
void pi_del(struct part_iter **_iter)
{
    if(!_iter || !*_iter)
	return;
    pi_dtor(*_iter);
    free(*_iter);
    *_iter = NULL;
}

/* pi_begin() - validate and and get proper iterator for a disk described by di */
struct part_iter *pi_begin(const struct disk_info *di, int flags)
{
    int gptprot, ret = -1;
    struct part_iter *iter;
    struct disk_dos_mbr *mbr = NULL;
    struct disk_gpt_header *gpth = NULL;
    struct disk_gpt_part_entry *gptl = NULL;

    /* Preallocate iterator */
    if (!(iter = pi_alloc()))
	goto bail;

    /* Read MBR */
    if (!(mbr = disk_read_sectors(di, 0, 1))) {
	error("Couldn't read the first disk sector.");
	goto bail;
    }

    /* Check for MBR magic */
    if (mbr->sig != disk_mbr_sig_magic) {
	warn("No MBR magic, treating disk as raw.");
	/* looks like RAW */
	ret = pi_ctor(iter, di, flags);
	goto bail;
    }

    /* Check for GPT protective MBR */
    gptprot = 0;
    for (size_t i = 0; i < 4; i++)
	gptprot |= (mbr->table[i].ostype == 0xEE);
    if (gptprot && !(flags & PIF_PREFMBR)) {
	if (!(gpth = disk_read_sectors(di, 1, 1))) {
	    error("Couldn't read potential GPT header.");
	    goto bail;
	}
    }

    if (gpth && gpth->rev.uint32 == 0x00010000 &&
	    !memcmp(gpth->sig, disk_gpt_sig_magic, sizeof(disk_gpt_sig_magic))) {
	/* looks like GPT v1.0 */
	uint64_t gpt_loff;	    /* offset to GPT partition list in sectors */
	uint64_t gpt_lsiz;	    /* size of GPT partition list in bytes */
	uint64_t gpt_lcnt;	    /* size of GPT partition in sectors */
#ifdef DEBUG
	dprintf("Looks like a GPT v1.0 disk.\n");
	disk_gpt_header_dump(gpth);
#endif
	/* Verify checksum, fallback to backup, then bail if invalid */
	if (gpt_check_hdr_crc(di, &gpth))
	    goto bail;

	gpt_loff = gpth->lba_table;
	gpt_lsiz = (uint64_t)gpth->part_size * gpth->part_count;
	gpt_lcnt = (gpt_lsiz + di->bps - 1) / di->bps;

	/*
	 * disk_read_sectors allows reading of max 255 sectors, so we use
	 * it as a sanity check base. EFI doesn't specify max (AFAIK).
	 * Apart from that, some extensive sanity checks.
	 */
	if (!(flags & PIF_RELAX) && (
		!gpt_loff || !gpt_lsiz || gpt_lcnt > 255u ||
		gpth->lba_first_usable > gpth->lba_last_usable ||
		!sane(gpt_loff, gpt_lcnt) ||
		gpt_loff + gpt_lcnt > gpth->lba_first_usable ||
		!sane(gpth->lba_last_usable, gpt_lcnt) ||
		gpth->lba_last_usable + gpt_lcnt >= gpth->lba_alt ||
		gpth->lba_alt >= di->lbacnt ||
		gpth->part_size < sizeof(struct disk_gpt_part_entry))) {
	    error("Invalid GPT header's values.");
	    goto bail;
	}
	if (!(gptl = disk_read_sectors(di, gpt_loff, gpt_lcnt))) {
	    error("Couldn't read GPT partition list.");
	    goto bail;
	}
	/* Check array checksum(s). */
	if (!valid_crc(gpth->table_chksum, (const uint8_t *)gptl, (unsigned int)gpt_lsiz)) {
	    warn("Checksum of the main GPT partition list is invalid, trying backup.");
	    free(gptl);
	    /* secondary array directly precedes secondary header */
	    if (!(gptl = disk_read_sectors(di, gpth->lba_alt - gpt_lcnt, gpt_lcnt))) {
		error("Couldn't read backup GPT partition list.");
		goto bail;
	    }
	    if (!valid_crc(gpth->table_chksum, (const uint8_t *)gptl, gpt_lsiz)) {
		error("Checksum of the backup GPT partition list is invalid, giving up.");
		goto bail;
	    }
	}
	/* looks like GPT */
	ret = pi_gpt_ctor(iter, di, flags, gpth, gptl);
    } else {
	/* looks like MBR */
	ret = pi_dos_ctor(iter, di, flags, mbr);
    }
bail:
    if (ret < 0)
	free(iter);
    free(mbr);
    free(gpth);
    free(gptl);

    return iter;
}

/* vim: set ts=8 sts=4 sw=4 noet: */
