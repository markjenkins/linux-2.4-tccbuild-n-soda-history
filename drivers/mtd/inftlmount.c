/* 
 * inftlmount.c -- INFTL mount code with extensive checks.
 *
 * Author: Greg Ungerer (gerg@snapgear.com)
 * (C) Copyright 2002-2003, Greg Ungerer (gerg@snapgear.com)
 *
 * Based heavily on the nftlmount.c code which is:
 * Author: Fabrice Bellard (fabrice.bellard@netgem.com) 
 * Copyright (C) 2000 Netgem S.A.
 *
 * $Id: inftlmount.c,v 1.9 2003/05/23 11:35:07 dwmw2 Exp $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define __NO_VERSION__
#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/errno.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nftl.h>
#include <linux/mtd/inftl.h>
#include <linux/mtd/compatmac.h>

char inftlmountrev[]="$Revision: 1.9 $";

/*
 * find_boot_record: Find the INFTL Media Header and its Spare copy which
 *	contains the various device information of the INFTL partition and
 *	Bad Unit Table. Update the PUtable[] table according to the Bad
 *	Unit Table. PUtable[] is used for management of Erase Unit in
 *	other routines in inftlcore.c and inftlmount.c.
 */
static int find_boot_record(struct INFTLrecord *inftl)
{
	struct inftl_unittail h1;
	//struct inftl_oob oob;
	unsigned int i, block, boot_record_count = 0;
	u8 buf[SECTORSIZE];
	struct INFTLMediaHeader *mh = &inftl->MediaHdr;
	struct INFTLPartition *ip;
	int retlen;

	DEBUG(MTD_DEBUG_LEVEL3, "INFTL: find_boot_record(inftl=0x%x)\n",
		(int)inftl);

        /*
	 * Assume logical EraseSize == physical erasesize for starting the
	 * scan. We'll sort it out later if we find a MediaHeader which says
	 * otherwise.
	 */
	inftl->EraseSize = inftl->mbd.mtd->erasesize;
        inftl->nb_blocks = inftl->mbd.mtd->size / inftl->EraseSize;

	inftl->MediaUnit = BLOCK_NIL;
	inftl->SpareMediaUnit = BLOCK_NIL;

	/* Search for a valid boot record */
	for (block = 0; block < inftl->nb_blocks; block++) {
		int ret;

		/*
		 * Check for BNAND header first. Then whinge if it's found
		 * but later checks fail.
		 */
		if ((ret = MTD_READ(inftl->mbd.mtd, block * inftl->EraseSize,
		    SECTORSIZE, &retlen, buf))) {
			static int warncount = 5;

			if (warncount) {
				printk(KERN_WARNING "INFTL: block read at 0x%x "
					"of mtd%d failed: %d\n",
					block * inftl->EraseSize,
					inftl->mbd.mtd->index, ret);
				if (!--warncount)
					printk(KERN_WARNING "INFTL: further "
						"failures for this block will "
						"not be printed\n");
			}
			continue;
		}

		if (retlen < 6 || memcmp(buf, "BNAND", 6)) {
			/* BNAND\0 not found. Continue */
			continue;
		}

		/* To be safer with BIOS, also use erase mark as discriminant */
		if ((ret = MTD_READOOB(inftl->mbd.mtd, block * inftl->EraseSize +
		    SECTORSIZE + 8, 8, &retlen, (char *)&h1) < 0)) {
			printk(KERN_WARNING "INFTL: ANAND header found at "
				"0x%x in mtd%d, but OOB data read failed "
				"(err %d)\n", block * inftl->EraseSize,
				inftl->mbd.mtd->index, ret);
			continue;
		}

		if (boot_record_count) {
			/*
			 * We've already processed one. So we just check if
			 * this one is the same as the first one we found.
			 */
			if (memcmp(mh, buf, sizeof(struct INFTLMediaHeader))) {
				printk(KERN_WARNING "INFTL: Media Headers at "
					"0x%x and 0x%x disagree.\n",
					inftl->MediaUnit * inftl->EraseSize,
					block * inftl->EraseSize);
				return -1;
			}
			if (boot_record_count == 1)
				inftl->SpareMediaUnit = block;

			/*
			 * Mark this boot record (INFTL MediaHeader) block as
			 * reserved.
			 */
			inftl->PUtable[block] = BLOCK_RESERVED;

			boot_record_count++;
			continue;
		}

		/*
		 * This is the first we've seen.
		 * Copy the media header structure into place.
		 */
		memcpy(mh, buf, sizeof(struct INFTLMediaHeader));
		mh->NoOfBootImageBlocks = le32_to_cpu(mh->NoOfBootImageBlocks);
		mh->NoOfBinaryPartitions = le32_to_cpu(mh->NoOfBinaryPartitions);
		mh->NoOfBDTLPartitions = le32_to_cpu(mh->NoOfBDTLPartitions);
		mh->BlockMultiplierBits = le32_to_cpu(mh->BlockMultiplierBits);
		mh->FormatFlags = le32_to_cpu(mh->FormatFlags);
		mh->PercentUsed = le32_to_cpu(mh->PercentUsed);

#ifdef CONFIG_MTD_DEBUG_VERBOSE
		if (CONFIG_MTD_DEBUG_VERBOSE >= 2) {
			printk("INFTL: Media Header ->\n"
				"    bootRecordID          = %s\n"
				"    NoOfBootImageBlocks   = %d\n"
				"    NoOfBinaryPartitions  = %d\n"
				"    NoOfBDTLPartitions    = %d\n"
				"    BlockMultiplerBits    = %d\n"
				"    FormatFlgs            = %d\n"
				"    OsakVersion           = 0x%x\n"
				"    PercentUsed           = %d\n",
				mh->bootRecordID, mh->NoOfBootImageBlocks,
				mh->NoOfBinaryPartitions,
				mh->NoOfBDTLPartitions,
				mh->BlockMultiplierBits, mh->FormatFlags,
				mh->OsakVersion, mh->PercentUsed);
		}
#endif

		if (mh->NoOfBDTLPartitions == 0) {
			printk(KERN_WARNING "INFTL: Media Header sanity check "
				"failed: NoOfBDTLPartitions (%d) == 0, "
				"must be at least 1\n", mh->NoOfBDTLPartitions);
			return -1;
		}

		if ((mh->NoOfBDTLPartitions + mh->NoOfBinaryPartitions) > 4) {
			printk(KERN_WARNING "INFTL: Media Header sanity check "
				"failed: Total Partitions (%d) > 4, "
				"BDTL=%d Binary=%d\n", mh->NoOfBDTLPartitions +
				mh->NoOfBinaryPartitions,
				mh->NoOfBDTLPartitions,
				mh->NoOfBinaryPartitions);
			return -1;
		}

		if (mh->BlockMultiplierBits > 1) {
			printk(KERN_WARNING "INFTL: sorry, we don't support "
				"UnitSizeFactor 0x%02x\n",
				mh->BlockMultiplierBits);
			return -1;
		} else if (mh->BlockMultiplierBits == 1) {
			printk(KERN_WARNING "INFTL: support for INFTL with "
				"UnitSizeFactor 0x%02x is experimental\n",
				mh->BlockMultiplierBits);
			inftl->EraseSize = inftl->mbd.mtd->erasesize <<
				(0xff - mh->BlockMultiplierBits);
			inftl->nb_blocks = inftl->mbd.mtd->size / inftl->EraseSize;
		}

		/* Scan the partitions */
		for (i = 0; (i < 4); i++) {
			ip = &mh->Partitions[i];
			ip->virtualUnits = le32_to_cpu(ip->virtualUnits);
			ip->firstUnit = le32_to_cpu(ip->firstUnit);
			ip->lastUnit = le32_to_cpu(ip->lastUnit);
			ip->flags = le32_to_cpu(ip->flags);
			ip->spareUnits = le32_to_cpu(ip->spareUnits);
			ip->Reserved0 = le32_to_cpu(ip->Reserved0);

#ifdef CONFIG_MTD_DEBUG_VERBOSE
			if (CONFIG_MTD_DEBUG_VERBOSE >= 2) {
				printk("    PARTITION[%d] ->\n"
					"        virtualUnits    = %d\n"
					"        firstUnit       = %d\n"
					"        lastUnit        = %d\n"
					"        flags           = 0x%x\n"
					"        spareUnits      = %d\n",
					i, ip->virtualUnits, ip->firstUnit,
					ip->lastUnit, ip->flags,
					ip->spareUnits);
			}
#endif

			if (ip->Reserved0 != ip->firstUnit) {
				struct erase_info *instr = &inftl->instr;

				/*
				 * 	Most likely this is using the
				 * 	undocumented qiuck mount feature.
				 * 	We don't support that, we will need
				 * 	to erase the hidden block for full
				 * 	compatibility.
				 */
				instr->addr = ip->Reserved0 * inftl->EraseSize;
				instr->len = inftl->EraseSize;
				MTD_ERASE(inftl->mbd.mtd, instr);
			}
			if ((ip->lastUnit - ip->firstUnit + 1) < ip->virtualUnits) {
				printk(KERN_WARNING "INFTL: Media Header "
					"Parition %d sanity check failed\n"
					"    firstUnit %d : lastUnit %d  >  "
					"virtualUnits %d\n", i, ip->lastUnit,
					ip->firstUnit, ip->Reserved0);
				return -1;
			}
			if (ip->Reserved1 != 0) {
				printk(KERN_WARNING "INFTL: Media Header "
					"Parition %d sanity check failed: "
					"Reserved1 %d != 0\n",
					i, ip->Reserved1);
				return -1;
			}

			if (ip->flags & INFTL_BDTL)
				break;
		}

		if (i >= 4) {
			printk(KERN_WARNING "INFTL: Media Header Parition "
				"sanity check failed:\n       No partition "
				"marked as Disk Partition\n");
			return -1;
		}

		inftl->nb_boot_blocks = ip->firstUnit;
		inftl->numvunits = ip->virtualUnits;
		if (inftl->numvunits > (inftl->nb_blocks -
		    inftl->nb_boot_blocks - 2)) {
			printk(KERN_WARNING "INFTL: Media Header sanity check "
				"failed:\n        numvunits (%d) > nb_blocks "
				"(%d) - nb_boot_blocks(%d) - 2\n",
				inftl->numvunits, inftl->nb_blocks,
				inftl->nb_boot_blocks);
			return -1;
		}
		
		inftl->mbd.size  = inftl->numvunits *
			(inftl->EraseSize / SECTORSIZE);

		/*
		 * Block count is set to last used EUN (we won't need to keep
		 * any meta-data past that point).
		 */
		inftl->firstEUN = ip->firstUnit;
		inftl->lastEUN = ip->lastUnit;
		inftl->nb_blocks = ip->lastUnit + 1;

		/* Memory alloc */
		inftl->PUtable = kmalloc(inftl->nb_blocks * sizeof(u16), GFP_KERNEL);
		if (!inftl->PUtable) {
			printk(KERN_WARNING "INFTL: allocation of PUtable "
				"failed (%d bytes)\n",
				inftl->nb_blocks * sizeof(u16));
			return -ENOMEM;
		}

		inftl->VUtable = kmalloc(inftl->nb_blocks * sizeof(u16), GFP_KERNEL);
		if (!inftl->VUtable) {
			kfree(inftl->PUtable);
			printk(KERN_WARNING "INFTL: allocation of VUtable "
				"failed (%d bytes)\n",
				inftl->nb_blocks * sizeof(u16));
			return -ENOMEM;
		}
		
		/* Mark the blocks before INFTL MediaHeader as reserved */
		for (i = 0; i < inftl->nb_boot_blocks; i++)
			inftl->PUtable[i] = BLOCK_RESERVED;
		/* Mark all remaining blocks as potentially containing data */
		for (; i < inftl->nb_blocks; i++)
			inftl->PUtable[i] = BLOCK_NOTEXPLORED;

		/* Mark this boot record (NFTL MediaHeader) block as reserved */
		inftl->PUtable[block] = BLOCK_RESERVED;

#if 0
		/* Read Bad Erase Unit Table and modify PUtable[] accordingly */
		for (i = 0; i < inftl->nb_blocks; i++) {
			if ((i & (SECTORSIZE - 1)) == 0) {
				/* read one sector for every SECTORSIZE of blocks */
				if ((ret = MTD_READECC(inftl->mbd.mtd,
				    block * inftl->EraseSize + i + SECTORSIZE,
				    SECTORSIZE, &retlen, buf,
				    (char *)&oob, NULL)) < 0) {
					printk(KERN_WARNING "INFTL: read of "
						"bad sector table failed "
						"(err %d)\n", ret);
					kfree(inftl->VUtable);
					kfree(inftl->PUtable);
					return -1;
				}
			}
			/* Mark the Bad Erase Unit as RESERVED in PUtable */
			if (buf[i & (SECTORSIZE - 1)] != 0xff)
				inftl->PUtable[i] = BLOCK_RESERVED;
		}
#endif

		inftl->MediaUnit = block;
		boot_record_count++;
	}
		
	return boot_record_count ? 0 : -1;
}

static int memcmpb(void *a, int c, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		if (c != ((unsigned char *)a)[i])
			return 1;
	}
	return 0;
}

/*
 * check_free_sector: check if a free sector is actually FREE,
 *	i.e. All 0xff in data and oob area.
 */
static int check_free_sectors(struct INFTLrecord *inftl, unsigned int address,
	int len, int check_oob)
{
	int i, retlen;
	u8 buf[SECTORSIZE];

	DEBUG(MTD_DEBUG_LEVEL3, "INFTL: check_free_sectors(inftl=0x%x,"
		"address=0x%x,len=%d,check_oob=%d)\n", (int)inftl,
		address, len, check_oob);

	for (i = 0; i < len; i += SECTORSIZE) {
		/*
		 * We want to read the sector without ECC check here since a
		 * free sector does not have ECC syndrome on it yet.
		 */
		if (MTD_READ(inftl->mbd.mtd, address, SECTORSIZE, &retlen, buf) < 0)
			return -1;
		if (memcmpb(buf, 0xff, SECTORSIZE) != 0)
			return -1;

		if (check_oob) {
			if (MTD_READOOB(inftl->mbd.mtd, address,
			    inftl->mbd.mtd->oobsize, &retlen, buf) < 0)
				return -1;
			if (memcmpb(buf, 0xff, inftl->mbd.mtd->oobsize) != 0)
				return -1;
		}
		address += SECTORSIZE;
	}

	return 0;
}

/*
 * INFTL_format: format a Erase Unit by erasing ALL Erase Zones in the Erase
 *		 Unit and Update INFTL metadata. Each erase operation is
 *		 checked with check_free_sectors.
 *
 * Return: 0 when succeed, -1 on error.
 *
 * ToDo: 1. Is it neceressary to check_free_sector after erasing ?? 
 *       2. UnitSizeFactor != 0xFF
 */
int INFTL_formatblock(struct INFTLrecord *inftl, int block)
{
	int retlen;
	struct inftl_unittail uci;
	struct erase_info *instr = &inftl->instr;

	DEBUG(MTD_DEBUG_LEVEL3, "INFTL: INFTL_formatblock(inftl=0x%x,"
		"block=%d)\n", (int)inftl, block);

	memset(instr, 0, sizeof(struct erase_info));

	/* Use async erase interface, test return code */
	instr->addr = block * inftl->EraseSize;
	instr->len = inftl->EraseSize;
	MTD_ERASE(inftl->mbd.mtd, instr);

	if (instr->state == MTD_ERASE_FAILED) {
		/*
		 * Could not format, FixMe: We should update the BadUnitTable 
		 * both in memory and on disk.
		 */
		printk(KERN_WARNING "INFTL: error while formatting block %d\n",
			block);
		return -1;
	}

	/*
	 * Check the "freeness" of Erase Unit before updating metadata.
	 * FixMe: is this check really necessary? Since we have check the
	 *        return code after the erase operation.
	 */
	if (check_free_sectors(inftl, instr->addr, inftl->EraseSize, 1) != 0)
		return -1;

	uci.EraseMark = cpu_to_le16(ERASE_MARK);
	uci.EraseMark1 = cpu_to_le16(ERASE_MARK);
	uci.Reserved[0] = 0;
	uci.Reserved[1] = 0;
	uci.Reserved[2] = 0;
	uci.Reserved[3] = 0;
	if (MTD_WRITEOOB(inftl->mbd.mtd, block * inftl->EraseSize + SECTORSIZE * 2 +
	    8, 8, &retlen, (char *)&uci) < 0)
		return -1;
	return 0;
}

/*
 * format_chain: Format an invalid Virtual Unit chain. It frees all the Erase
 *	Units in a Virtual Unit Chain, i.e. all the units are disconnected.
 *
 *	Since the chain is invalid then we will have to erase it from its
 *	head (normally for INFTL we go from the oldest). But if it has a
 *	loop then there is no oldest...
 */
static void format_chain(struct INFTLrecord *inftl, unsigned int first_block)
{
	unsigned int block = first_block, block1;

	printk(KERN_WARNING "INFTL: formatting chain at block %d\n",
		first_block);

	for (;;) {
		block1 = inftl->PUtable[block];

		printk(KERN_WARNING "INFTL: formatting block %d\n", block);
		if (INFTL_formatblock(inftl, block) < 0) {
			/*
			 * Cannot format !!!! Mark it as Bad Unit,
			 * FixMe: update the BadUnitTable on disk.
			 */
			inftl->PUtable[block] = BLOCK_RESERVED;
		} else {
			inftl->PUtable[block] = BLOCK_FREE;
		}

		/* Goto next block on the chain */
		block = block1;

		if (block == BLOCK_NIL || block >= inftl->lastEUN)
			break;
	}
}

void INFTL_dumptables(struct INFTLrecord *s)
{
	int i;

	printk("-------------------------------------------"
		"----------------------------------\n");

	printk("VUtable[%d] ->", s->nb_blocks);
	for (i = 0; i < s->nb_blocks; i++) {
		if ((i % 8) == 0)
			printk("\n%04x: ", i);
		printk("%04x ", s->VUtable[i]);
	}

	printk("\n-------------------------------------------"
		"----------------------------------\n");

	printk("PUtable[%d-%d=%d] ->", s->firstEUN, s->lastEUN, s->nb_blocks);
	for (i = 0; i <= s->lastEUN; i++) {
		if ((i % 8) == 0)
			printk("\n%04x: ", i);
		printk("%04x ", s->PUtable[i]);
	}

	printk("\n-------------------------------------------"
		"----------------------------------\n");

	printk("INFTL ->\n"
		"  EraseSize       = %d\n"
		"  h/s/c           = %d/%d/%d\n"
		"  numvunits       = %d\n"
		"  firstEUN        = %d\n"
		"  lastEUN         = %d\n"
		"  numfreeEUNs     = %d\n"
		"  LastFreeEUN     = %d\n"
		"  nb_blocks       = %d\n"
		"  nb_boot_blocks  = %d",
		s->EraseSize, s->heads, s->sectors, s->cylinders,
		s->numvunits, s->firstEUN, s->lastEUN, s->numfreeEUNs,
		s->LastFreeEUN, s->nb_blocks, s->nb_boot_blocks);

	printk("\n-------------------------------------------"
		"----------------------------------\n");
}

void INFTL_dumpVUchains(struct INFTLrecord *s)
{
	int logical, block, i;

	printk("-------------------------------------------"
		"----------------------------------\n");

	printk("INFTL Virtual Unit Chains:\n");
	for (logical = 0; logical < s->nb_blocks; logical++) {
		block = s->VUtable[logical];
		if (block > s->nb_blocks)
			continue;
		printk("  LOGICAL %d --> %d ", logical, block);
		for (i = 0; i < s->nb_blocks; i++) {
			if (s->PUtable[block] == BLOCK_NIL)
				break;
			block = s->PUtable[block];
			printk("%d ", block);
		}
		printk("\n");
	}

	printk("-------------------------------------------"
		"----------------------------------\n");
}

int INFTL_mount(struct INFTLrecord *s)
{
	unsigned int block, first_block, prev_block, last_block;
	unsigned int first_logical_block, logical_block, erase_mark;
	int chain_length, do_format_chain;
	struct inftl_unithead1 h0;
	struct inftl_unittail h1;
	int i, retlen;
	u8 *ANACtable, ANAC;

	DEBUG(MTD_DEBUG_LEVEL3, "INFTL: INFTL_mount(inftl=0x%x)\n", (int)s);

	/* Search for INFTL MediaHeader and Spare INFTL Media Header */
	if (find_boot_record(s) < 0) {
		printk(KERN_WARNING "INFTL: could not find valid boot record?\n");
		return -1;
	}

	/* Init the logical to physical table */
	for (i = 0; i < s->nb_blocks; i++)
		s->VUtable[i] = BLOCK_NIL;

	logical_block = block = BLOCK_NIL;

	/* Temporary buffer to store ANAC numbers. */
	ANACtable = kmalloc(s->nb_blocks * sizeof(u8), GFP_KERNEL);
	memset(ANACtable, 0, s->nb_blocks);

	/*
	 * First pass is to explore each physical unit, and construct the
	 * virtual chains that exist (newest physical unit goes into VUtable).
	 * Any block that is in any way invalid will be left in the
	 * NOTEXPLORED state. Then at the end we will try to format it and
	 * mark it as free.
	 */
	DEBUG(MTD_DEBUG_LEVEL3, "INFTL: pass 1, explore each unit\n");
	for (first_block = s->firstEUN; first_block <= s->lastEUN; first_block++) {
		if (s->PUtable[first_block] != BLOCK_NOTEXPLORED)
			continue;

		do_format_chain = 0;
		first_logical_block = BLOCK_NIL;
		last_block = BLOCK_NIL;
		block = first_block;

		for (chain_length = 0; ; chain_length++) {

			if ((chain_length == 0) && 
			    (s->PUtable[block] != BLOCK_NOTEXPLORED)) {
				/* Nothing to do here, onto next block */
				break;
			}

			if (MTD_READOOB(s->mbd.mtd, block * s->EraseSize + 8,
			    8, &retlen, (char *)&h0) < 0 ||
			    MTD_READOOB(s->mbd.mtd, block * s->EraseSize +
			    2 * SECTORSIZE + 8, 8, &retlen, (char *)&h1) < 0) {
				/* Should never happen? */
				do_format_chain++;
				break;
			}

			logical_block = le16_to_cpu(h0.virtualUnitNo);
			prev_block = le16_to_cpu(h0.prevUnitNo);
			erase_mark = le16_to_cpu((h1.EraseMark | h1.EraseMark1));
			ANACtable[block] = h0.ANAC;

			/* Previous block is relative to start of Partition */
			if (prev_block < s->nb_blocks)
				prev_block += s->firstEUN;

			/* Already explored paritial chain? */
			if (s->PUtable[block] != BLOCK_NOTEXPLORED) {
				/* Check if chain for this logical */
				if (logical_block == first_logical_block) {
					if (last_block != BLOCK_NIL)
						s->PUtable[last_block] = block;
				}
				break;
			}

			/* Check for invalid block */
			if (erase_mark != ERASE_MARK) {
				printk(KERN_WARNING "INFTL: corrupt block %d "
					"in chain %d, chain length %d, erase "
					"mark 0x%x?\n", block, first_block,
					chain_length, erase_mark);
				/*
				 * Assume end of chain, probably incomplete
				 * fold/erase...
				 */
				if (chain_length == 0)
					do_format_chain++;
				break;
			}

			/* Check for it being free already then... */
			if ((logical_block == BLOCK_FREE) ||
			    (logical_block == BLOCK_NIL)) {
				s->PUtable[block] = BLOCK_FREE;
				break;
			}

			/* Sanity checks on block numbers */
			if ((logical_block >= s->nb_blocks) ||
			    ((prev_block >= s->nb_blocks) &&
			     (prev_block != BLOCK_NIL))) {
				if (chain_length > 0) {
					printk(KERN_WARNING "INFTL: corrupt "
						"block %d in chain %d?\n",
						block, first_block);
					do_format_chain++;
				}
				break;
			}

			if (first_logical_block == BLOCK_NIL) {
				first_logical_block = logical_block;
			} else {
				if (first_logical_block != logical_block) {
					/* Normal for folded chain... */
					break;
				}
			}

			/*
			 * Current block is valid, so if we followed a virtual
			 * chain to get here then we can set the previous
			 * block pointer in our PUtable now. Then move onto
			 * the previous block in the chain.
			 */
			s->PUtable[block] = BLOCK_NIL;
			if (last_block != BLOCK_NIL)
				s->PUtable[last_block] = block;
			last_block = block;
			block = prev_block;

			/* Check for end of chain */
			if (block == BLOCK_NIL)
				break;

			/* Validate next block before following it... */
			if (block > s->lastEUN) {
				printk(KERN_WARNING "INFTL: invalid previous "
					"block %d in chain %d?\n", block,
					first_block);
				do_format_chain++;
				break;
			}
		}

		if (do_format_chain) {
			format_chain(s, first_block);
			continue;
		}

		/*
		 * Looks like a valid chain then. It may not really be the
		 * newest block in the chain, but it is the newest we have
		 * found so far. We might update it in later iterations of
		 * this loop if we find something newer.
		 */
		s->VUtable[first_logical_block] = first_block;
		logical_block = BLOCK_NIL;
	}

#ifdef CONFIG_MTD_DEBUG_VERBOSE
	if (CONFIG_MTD_DEBUG_VERBOSE >= 2)
		INFTL_dumptables(s);
#endif

	/*
	 * Second pass, check for infinite loops in chains. These are
	 * possible because we don't update the previous pointers when
	 * we fold chains. No big deal, just fix them up in PUtable.
	 */
	DEBUG(MTD_DEBUG_LEVEL3, "INFTL: pass 2, validate virtual chains\n");
	for (logical_block = 0; logical_block < s->numvunits; logical_block++) {
		block = s->VUtable[logical_block];
		last_block = BLOCK_NIL;

		/* Check for free/reserved/nil */
		if (block >= BLOCK_RESERVED)
			continue;

		ANAC = ANACtable[block];
		for (i = 0; i < s->numvunits; i++) {
			if (s->PUtable[block] == BLOCK_NIL)
				break;
			if (s->PUtable[block] > s->lastEUN) {
				printk(KERN_WARNING "INFTL: invalid prev %d, "
					"in virtual chain %d\n",
					s->PUtable[block], logical_block);
				s->PUtable[block] = BLOCK_NIL;
					
			}
			if (ANACtable[block] != ANAC) {
				/*
				 * Chain must point back to itself. This is ok,
				 * but we will need adjust the tables with this
				 * newest block and oldest block.
				 */
				s->VUtable[logical_block] = block;
				s->PUtable[last_block] = BLOCK_NIL;
				break;
			}

			ANAC--;
			last_block = block;
			block = s->PUtable[block];
		}

		if (i >= s->nb_blocks) {
			/*
			 * Uhoo, infinite chain with valid ANACS!
			 * Format whole chain...
			 */
			format_chain(s, first_block);
		}
	}

#ifdef CONFIG_MTD_DEBUG_VERBOSE
	if (CONFIG_MTD_DEBUG_VERBOSE >= 2)
		INFTL_dumptables(s);
	if (CONFIG_MTD_DEBUG_VERBOSE >= 2)
		INFTL_dumpVUchains(s);
#endif

	/*
	 * Third pass, format unreferenced blocks and init free block count.
	 */
	s->numfreeEUNs = 0;
	s->LastFreeEUN = BLOCK_NIL;

	DEBUG(MTD_DEBUG_LEVEL3, "INFTL: pass 3, format unused blocks\n");
	for (block = s->firstEUN; block <= s->lastEUN; block++) {
		if (s->PUtable[block] == BLOCK_NOTEXPLORED) {
			printk("INFTL: unreferenced block %d, formatting it\n",
				block);
			if (INFTL_formatblock(s, block) < 0)
				s->PUtable[block] = BLOCK_RESERVED;
			else
				s->PUtable[block] = BLOCK_FREE;
		}
		if (s->PUtable[block] == BLOCK_FREE) {
			s->numfreeEUNs++;
			if (s->LastFreeEUN == BLOCK_NIL)
				s->LastFreeEUN = block;
		}
	}

	kfree(ANACtable);
	return 0;
}
