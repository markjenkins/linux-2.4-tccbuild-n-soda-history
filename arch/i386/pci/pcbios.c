/*
 * BIOS32 and PCI BIOS handling.
 */

#include <linux/pci.h>
#include <linux/init.h>
#include "pci.h"


#define PCIBIOS_PCI_FUNCTION_ID 	0xb1XX
#define PCIBIOS_PCI_BIOS_PRESENT 	0xb101
#define PCIBIOS_FIND_PCI_DEVICE		0xb102
#define PCIBIOS_FIND_PCI_CLASS_CODE	0xb103
#define PCIBIOS_GENERATE_SPECIAL_CYCLE	0xb106
#define PCIBIOS_READ_CONFIG_BYTE	0xb108
#define PCIBIOS_READ_CONFIG_WORD	0xb109
#define PCIBIOS_READ_CONFIG_DWORD	0xb10a
#define PCIBIOS_WRITE_CONFIG_BYTE	0xb10b
#define PCIBIOS_WRITE_CONFIG_WORD	0xb10c
#define PCIBIOS_WRITE_CONFIG_DWORD	0xb10d
#define PCIBIOS_GET_ROUTING_OPTIONS	0xb10e
#define PCIBIOS_SET_PCI_HW_INT		0xb10f

/* BIOS32 signature: "_32_" */
#define BIOS32_SIGNATURE	(('_' << 0) + ('3' << 8) + ('2' << 16) + ('_' << 24))

/* PCI signature: "PCI " */
#define PCI_SIGNATURE		(('P' << 0) + ('C' << 8) + ('I' << 16) + (' ' << 24))

/* PCI service signature: "$PCI" */
#define PCI_SERVICE		(('$' << 0) + ('P' << 8) + ('C' << 16) + ('I' << 24))

/* PCI BIOS hardware mechanism flags */
#define PCIBIOS_HW_TYPE1		0x01
#define PCIBIOS_HW_TYPE2		0x02
#define PCIBIOS_HW_TYPE1_SPEC		0x10
#define PCIBIOS_HW_TYPE2_SPEC		0x20

/*
 * This is the standard structure used to identify the entry point
 * to the BIOS32 Service Directory, as documented in
 * 	Standard BIOS 32-bit Service Directory Proposal
 * 	Revision 0.4 May 24, 1993
 * 	Phoenix Technologies Ltd.
 *	Norwood, MA
 * and the PCI BIOS specification.
 */

union bios32 {
	struct {
		unsigned long signature;	/* _32_ */
		unsigned long entry;		/* 32 bit physical address */
		unsigned char revision;		/* Revision level, 0 */
		unsigned char length;		/* Length in paragraphs should be 01 */
		unsigned char checksum;		/* All bytes must add up to zero */
		unsigned char reserved[5]; 	/* Must be zero */
	} fields;
	char chars[16];
};

/*
 * Physical address of the service directory.  I don't know if we're
 * allowed to have more than one of these or not, so just in case
 * we'll make pcibios_present() take a memory start parameter and store
 * the array there.
 */

static struct {
	unsigned long address;
	unsigned short segment;
} bios32_indirect = { 0, __KERNEL_CS };

/*
 * Returns the entry point for the given service, NULL on error
 */

static unsigned long bios32_service(unsigned long service)
{
	unsigned char return_code;	/* %al */
	unsigned long address;		/* %ebx */
	unsigned long length;		/* %ecx */
	unsigned long entry;		/* %edx */
	unsigned long flags;

	local_save_flags(flags); local_irq_disable();
	__asm__("lcall *(%%edi); cld"
		: "=a" (return_code),
		  "=b" (address),
		  "=c" (length),
		  "=d" (entry)
		: "0" (service),
		  "1" (0),
		  "D" (&bios32_indirect));
	local_irq_restore(flags);

	switch (return_code) {
		case 0:
			return address + entry;
		case 0x80:	/* Not present */
			printk(KERN_WARNING "bios32_service(0x%lx): not present\n", service);
			return 0;
		default: /* Shouldn't happen */
			printk(KERN_WARNING "bios32_service(0x%lx): returned 0x%x -- BIOS bug!\n",
				service, return_code);
			return 0;
	}
}

static struct {
	unsigned long address;
	unsigned short segment;
} pci_indirect = { 0, __KERNEL_CS };

static int pci_bios_present;

static int __devinit check_pcibios(void)
{
	u32 signature, eax, ebx, ecx;
	u8 status, major_ver, minor_ver, hw_mech;
	unsigned long flags, pcibios_entry;

	if ((pcibios_entry = bios32_service(PCI_SERVICE))) {
		pci_indirect.address = pcibios_entry + PAGE_OFFSET;

		local_save_flags(flags); local_irq_disable();
		__asm__(
			"lcall *(%%edi); cld\n\t"
			"jc 1f\n\t"
			"xor %%ah, %%ah\n"
			"1:"
			: "=d" (signature),
			  "=a" (eax),
			  "=b" (ebx),
			  "=c" (ecx)
			: "1" (PCIBIOS_PCI_BIOS_PRESENT),
			  "D" (&pci_indirect)
			: "memory");
		local_irq_restore(flags);

		status = (eax >> 8) & 0xff;
		hw_mech = eax & 0xff;
		major_ver = (ebx >> 8) & 0xff;
		minor_ver = ebx & 0xff;
		if (pcibios_last_bus < 0)
			pcibios_last_bus = ecx & 0xff;
		DBG("PCI: BIOS probe returned s=%02x hw=%02x ver=%02x.%02x l=%02x\n",
			status, hw_mech, major_ver, minor_ver, pcibios_last_bus);
		if (status || signature != PCI_SIGNATURE) {
			printk (KERN_ERR "PCI: BIOS BUG #%x[%08x] found\n",
				status, signature);
			return 0;
		}
		printk(KERN_INFO "PCI: PCI BIOS revision %x.%02x entry at 0x%lx, last bus=%d\n",
			major_ver, minor_ver, pcibios_entry, pcibios_last_bus);
#ifdef CONFIG_PCI_DIRECT
		if (!(hw_mech & PCIBIOS_HW_TYPE1))
			pci_probe &= ~PCI_PROBE_CONF1;
		if (!(hw_mech & PCIBIOS_HW_TYPE2))
			pci_probe &= ~PCI_PROBE_CONF2;
#endif
		return 1;
	}
	return 0;
}

static int __devinit pci_bios_find_device (unsigned short vendor, unsigned short device_id,
					unsigned short index, unsigned char *bus, unsigned char *device_fn)
{
	unsigned short bx;
	unsigned short ret;

	__asm__("lcall *(%%edi); cld\n\t"
		"jc 1f\n\t"
		"xor %%ah, %%ah\n"
		"1:"
		: "=b" (bx),
		  "=a" (ret)
		: "1" (PCIBIOS_FIND_PCI_DEVICE),
		  "c" (device_id),
		  "d" (vendor),
		  "S" ((int) index),
		  "D" (&pci_indirect));
	*bus = (bx >> 8) & 0xff;
	*device_fn = bx & 0xff;
	return (int) (ret & 0xff00) >> 8;
}

static int pci_bios_read (int seg, int bus, int dev, int fn, int reg, int len, u32 *value)
{
	unsigned long result = 0;
	unsigned long flags;
	unsigned long bx = ((bus << 8) | (dev << 3) | fn);

	if (!value || (bus > 255) || (dev > 31) || (fn > 7) || (reg > 255))
		return -EINVAL;

	spin_lock_irqsave(&pci_config_lock, flags);

	switch (len) {
	case 1:
		__asm__("lcall *(%%esi); cld\n\t"
			"jc 1f\n\t"
			"xor %%ah, %%ah\n"
			"1:"
			: "=c" (*value),
			  "=a" (result)
			: "1" (PCIBIOS_READ_CONFIG_BYTE),
			  "b" (bx),
			  "D" ((long)reg),
			  "S" (&pci_indirect));
		break;
	case 2:
		__asm__("lcall *(%%esi); cld\n\t"
			"jc 1f\n\t"
			"xor %%ah, %%ah\n"
			"1:"
			: "=c" (*value),
			  "=a" (result)
			: "1" (PCIBIOS_READ_CONFIG_WORD),
			  "b" (bx),
			  "D" ((long)reg),
			  "S" (&pci_indirect));
		break;
	case 4:
		__asm__("lcall *(%%esi); cld\n\t"
			"jc 1f\n\t"
			"xor %%ah, %%ah\n"
			"1:"
			: "=c" (*value),
			  "=a" (result)
			: "1" (PCIBIOS_READ_CONFIG_DWORD),
			  "b" (bx),
			  "D" ((long)reg),
			  "S" (&pci_indirect));
		break;
	}

	spin_unlock_irqrestore(&pci_config_lock, flags);

	return (int)((result & 0xff00) >> 8);
}

static int pci_bios_write (int seg, int bus, int dev, int fn, int reg, int len, u32 value)
{
	unsigned long result = 0;
	unsigned long flags;
	unsigned long bx = ((bus << 8) | (dev << 3) | fn);

	if ((bus > 255) || (dev > 31) || (fn > 7) || (reg > 255)) 
		return -EINVAL;

	spin_lock_irqsave(&pci_config_lock, flags);

	switch (len) {
	case 1:
		__asm__("lcall *(%%esi); cld\n\t"
			"jc 1f\n\t"
			"xor %%ah, %%ah\n"
			"1:"
			: "=a" (result)
			: "0" (PCIBIOS_WRITE_CONFIG_BYTE),
			  "c" (value),
			  "b" (bx),
			  "D" ((long)reg),
			  "S" (&pci_indirect));
		break;
	case 2:
		__asm__("lcall *(%%esi); cld\n\t"
			"jc 1f\n\t"
			"xor %%ah, %%ah\n"
			"1:"
			: "=a" (result)
			: "0" (PCIBIOS_WRITE_CONFIG_WORD),
			  "c" (value),
			  "b" (bx),
			  "D" ((long)reg),
			  "S" (&pci_indirect));
		break;
	case 4:
		__asm__("lcall *(%%esi); cld\n\t"
			"jc 1f\n\t"
			"xor %%ah, %%ah\n"
			"1:"
			: "=a" (result)
			: "0" (PCIBIOS_WRITE_CONFIG_DWORD),
			  "c" (value),
			  "b" (bx),
			  "D" ((long)reg),
			  "S" (&pci_indirect));
		break;
	}

	spin_unlock_irqrestore(&pci_config_lock, flags);

	return (int)((result & 0xff00) >> 8);
}

static int pci_bios_read_config_byte(struct pci_dev *dev, int where, u8 *value)
{
	int result; 
	u32 data;

	if (!value) 
		return -EINVAL;

	result = pci_bios_read(0, dev->bus->number, PCI_SLOT(dev->devfn), 
		PCI_FUNC(dev->devfn), where, 1, &data);

	*value = (u8)data;

	return result;
}

static int pci_bios_read_config_word(struct pci_dev *dev, int where, u16 *value)
{
	int result; 
	u32 data;

	if (!value) 
		return -EINVAL;

	result = pci_bios_read(0, dev->bus->number, PCI_SLOT(dev->devfn), 
		PCI_FUNC(dev->devfn), where, 2, &data);

	*value = (u16)data;

	return result;
}

static int pci_bios_read_config_dword(struct pci_dev *dev, int where, u32 *value)
{
	if (!value) 
		return -EINVAL;
	
	return pci_bios_read(0, dev->bus->number, PCI_SLOT(dev->devfn), 
		PCI_FUNC(dev->devfn), where, 4, value);
}

static int pci_bios_write_config_byte(struct pci_dev *dev, int where, u8 value)
{
	return pci_bios_write(0, dev->bus->number, PCI_SLOT(dev->devfn), 
		PCI_FUNC(dev->devfn), where, 1, value);
}

static int pci_bios_write_config_word(struct pci_dev *dev, int where, u16 value)
{
	return pci_bios_write(0, dev->bus->number, PCI_SLOT(dev->devfn), 
		PCI_FUNC(dev->devfn), where, 2, value);
}

static int pci_bios_write_config_dword(struct pci_dev *dev, int where, u32 value)
{
	return pci_bios_write(0, dev->bus->number, PCI_SLOT(dev->devfn), 
		PCI_FUNC(dev->devfn), where, 4, value);
}


/*
 * Function table for BIOS32 access
 */

static struct pci_ops pci_bios_access = {
      pci_bios_read_config_byte,
      pci_bios_read_config_word,
      pci_bios_read_config_dword,
      pci_bios_write_config_byte,
      pci_bios_write_config_word,
      pci_bios_write_config_dword
};

/*
 * Try to find PCI BIOS.
 */

static struct pci_ops * __devinit pci_find_bios(void)
{
	union bios32 *check;
	unsigned char sum;
	int i, length;

	/*
	 * Follow the standard procedure for locating the BIOS32 Service
	 * directory by scanning the permissible address range from
	 * 0xe0000 through 0xfffff for a valid BIOS32 structure.
	 */

	for (check = (union bios32 *) __va(0xe0000);
	     check <= (union bios32 *) __va(0xffff0);
	     ++check) {
		if (check->fields.signature != BIOS32_SIGNATURE)
			continue;
		length = check->fields.length * 16;
		if (!length)
			continue;
		sum = 0;
		for (i = 0; i < length ; ++i)
			sum += check->chars[i];
		if (sum != 0)
			continue;
		if (check->fields.revision != 0) {
			printk("PCI: unsupported BIOS32 revision %d at 0x%p\n",
				check->fields.revision, check);
			continue;
		}
		DBG("PCI: BIOS32 Service Directory structure at 0x%p\n", check);
		if (check->fields.entry >= 0x100000) {
			printk("PCI: BIOS32 entry (0x%p) in high memory, cannot use.\n", check);
			return NULL;
		} else {
			unsigned long bios32_entry = check->fields.entry;
			DBG("PCI: BIOS32 Service Directory entry at 0x%lx\n", bios32_entry);
			bios32_indirect.address = bios32_entry + PAGE_OFFSET;
			if (check_pcibios())
				return &pci_bios_access;
		}
		break;	/* Hopefully more than one BIOS32 cannot happen... */
	}

	return NULL;
}

/*
 * Sort the device list according to PCI BIOS. Nasty hack, but since some
 * fool forgot to define the `correct' device order in the PCI BIOS specs
 * and we want to be (possibly bug-to-bug ;-]) compatible with older kernels
 * which used BIOS ordering, we are bound to do this...
 */

void __devinit pcibios_sort(void)
{
	LIST_HEAD(sorted_devices);
	struct list_head *ln;
	struct pci_dev *dev, *d;
	int idx, found;
	unsigned char bus, devfn;

	DBG("PCI: Sorting device list...\n");
	while (!list_empty(&pci_devices)) {
		ln = pci_devices.next;
		dev = pci_dev_g(ln);
		idx = found = 0;
		while (pci_bios_find_device(dev->vendor, dev->device, idx, &bus, &devfn) == PCIBIOS_SUCCESSFUL) {
			idx++;
			for (ln=pci_devices.next; ln != &pci_devices; ln=ln->next) {
				d = pci_dev_g(ln);
				if (d->bus->number == bus && d->devfn == devfn) {
					list_del(&d->global_list);
					list_add_tail(&d->global_list, &sorted_devices);
					if (d == dev)
						found = 1;
					break;
				}
			}
			if (ln == &pci_devices) {
				printk(KERN_WARNING "PCI: BIOS reporting unknown device %02x:%02x\n", bus, devfn);
				/*
				 * We must not continue scanning as several buggy BIOSes
				 * return garbage after the last device. Grr.
				 */
				break;
			}
		}
		if (!found) {
			printk(KERN_WARNING "PCI: Device %02x:%02x not found by BIOS\n",
				dev->bus->number, dev->devfn);
			list_del(&dev->global_list);
			list_add_tail(&dev->global_list, &sorted_devices);
		}
	}
	list_splice(&sorted_devices, &pci_devices);
}

/*
 *  BIOS Functions for IRQ Routing
 */

struct irq_routing_options {
	u16 size;
	struct irq_info *table;
	u16 segment;
} __attribute__((packed));

struct irq_routing_table * __devinit pcibios_get_irq_routing_table(void)
{
	struct irq_routing_options opt;
	struct irq_routing_table *rt = NULL;
	int ret, map;
	unsigned long page;

	if (!pci_bios_present)
		return NULL;
	page = __get_free_page(GFP_KERNEL);
	if (!page)
		return NULL;
	opt.table = (struct irq_info *) page;
	opt.size = PAGE_SIZE;
	opt.segment = __KERNEL_DS;

	DBG("PCI: Fetching IRQ routing table... ");
	__asm__("push %%es\n\t"
		"push %%ds\n\t"
		"pop  %%es\n\t"
		"lcall *(%%esi); cld\n\t"
		"pop %%es\n\t"
		"jc 1f\n\t"
		"xor %%ah, %%ah\n"
		"1:"
		: "=a" (ret),
		  "=b" (map)
		: "0" (PCIBIOS_GET_ROUTING_OPTIONS),
		  "1" (0),
		  "D" ((long) &opt),
		  "S" (&pci_indirect));
	DBG("OK  ret=%d, size=%d, map=%x\n", ret, opt.size, map);
	if (ret & 0xff00)
		printk(KERN_ERR "PCI: Error %02x when fetching IRQ routing table.\n", (ret >> 8) & 0xff);
	else if (opt.size) {
		rt = kmalloc(sizeof(struct irq_routing_table) + opt.size, GFP_KERNEL);
		if (rt) {
			memset(rt, 0, sizeof(struct irq_routing_table));
			rt->size = opt.size + sizeof(struct irq_routing_table);
			rt->exclusive_irqs = map;
			memcpy(rt->slots, (void *) page, opt.size);
			printk(KERN_INFO "PCI: Using BIOS Interrupt Routing Table\n");
		}
	}
	free_page(page);
	return rt;
}


int pcibios_set_irq_routing(struct pci_dev *dev, int pin, int irq)
{
	int ret;

	__asm__("lcall *(%%esi); cld\n\t"
		"jc 1f\n\t"
		"xor %%ah, %%ah\n"
		"1:"
		: "=a" (ret)
		: "0" (PCIBIOS_SET_PCI_HW_INT),
		  "b" ((dev->bus->number << 8) | dev->devfn),
		  "c" ((irq << 8) | (pin + 10)),
		  "S" (&pci_indirect));
	return !(ret & 0xff00);
}

static int __init pci_pcbios_init(void)
{
	if ((pci_probe & PCI_PROBE_BIOS) 
		&& ((pci_root_ops = pci_find_bios()))) {
		pci_probe |= PCI_BIOS_SORT;
		pci_bios_present = 1;
		pci_config_read = pci_bios_read;
		pci_config_write = pci_bios_write;
	}
	return 0;
}

arch_initcall(pci_pcbios_init);
