#ifndef _SPARC64_TLBFLUSH_H
#define _SPARC64_TLBFLUSH_H

#include <linux/config.h>
#include <linux/mm.h>

/* TLB flush operations. */

extern void __flush_tlb_all(void);
extern void __flush_tlb_mm(unsigned long context, unsigned long r);
extern void __flush_tlb_range(unsigned long context, unsigned long start,
			      unsigned long r, unsigned long end,
			      unsigned long pgsz, unsigned long size);
extern void __flush_tlb_page(unsigned long context, unsigned long page, unsigned long r);

#ifndef CONFIG_SMP

#define flush_tlb_all()		__flush_tlb_all()

#define flush_tlb_mm(__mm) \
do { if(CTX_VALID((__mm)->context)) \
	__flush_tlb_mm(CTX_HWBITS((__mm)->context), SECONDARY_CONTEXT); \
} while(0)

#define flush_tlb_range(__vma, start, end) \
do { if(CTX_VALID((__vma)->vm_mm->context)) { \
	unsigned long __start = (start)&PAGE_MASK; \
	unsigned long __end = PAGE_ALIGN(end); \
	__flush_tlb_range(CTX_HWBITS((__vma)->vm_mm->context), __start, \
			  SECONDARY_CONTEXT, __end, PAGE_SIZE, \
			  (__end - __start)); \
     } \
} while(0)

#define flush_tlb_page(vma, page) \
do { struct mm_struct *__mm = (vma)->vm_mm; \
     if(CTX_VALID(__mm->context)) \
	__flush_tlb_page(CTX_HWBITS(__mm->context), (page)&PAGE_MASK, \
			 SECONDARY_CONTEXT); \
} while(0)

#else /* CONFIG_SMP */

extern void smp_flush_tlb_all(void);
extern void smp_flush_tlb_mm(struct mm_struct *mm);
extern void smp_flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
				unsigned long end);
extern void smp_flush_tlb_page(struct mm_struct *mm, unsigned long page);

#define flush_cache_all()	smp_flush_cache_all()
#define flush_tlb_all()		smp_flush_tlb_all()
#define flush_tlb_mm(mm)	smp_flush_tlb_mm(mm)
#define flush_tlb_range(vma, start, end) \
	smp_flush_tlb_range(vma, start, end)
#define flush_tlb_page(vma, page) \
	smp_flush_tlb_page((vma)->vm_mm, page)

#endif /* ! CONFIG_SMP */

#endif /* _SPARC64_TLBFLUSH_H */
