#include <nautilus/nautilus.h>
#include <nautilus/spinlock.h>
#include <nautilus/paging.h>
#include <nautilus/thread.h>
#include <nautilus/shell.h>
#include <nautilus/cpu.h>

#include <nautilus/aspace.h>

#define REGION_FORMAT "(VA=0x%p to PA=0x%p, len=%lx, prot=%lx)"
#define REGION(r) (r)->va_start, (r)->pa_start, (r)->len_bytes, (r)->protect.flags


static int paging_sanity(char *_buf, void* _priv) {

#define LEN_1KB (0x400UL)
#define LEN_4KB (0x1000UL)
#define LEN_16KB (0x10000UL)
#define LEN_256KB (0x40000UL)
#define LEN_512KB (0x80000UL)

#define LEN_1MB (0x100000UL)
#define LEN_2MB (0x200000UL)
#define LEN_4MB (0x400000UL)
#define LEN_6MB (0x600000UL)
#define LEN_8MB (0x800000UL)
#define LEN_16MB (0x1000000UL)

#define LEN_1GB (0x40000000UL)
#define LEN_4GB (0x100000000UL)

#define ADDR_1GB ((void *) 0x40000000UL)
#define ADDR_4GB ((void *) 0x100000000UL)
#define ADDR_8GB ((void *) 0x200000000UL)
#define ADDR_12GB ((void *) 0x300000000UL)
#define ADDR_16GB ((void *) 0x400000000UL)
#define ADDR_UPPER ((void *) 0xffff800000000000UL)

    int test_failed = 0;
    nk_vc_printf("Running Paging sanity Check!\n");
    

    nk_aspace_characteristics_t c;

    if (nk_aspace_query("paging",&c)) {
        nk_vc_printf("failed to find paging implementation\n");
        test_failed = 1;
        goto no_paging_exit;
    }
    
    // create a new address space for this shell thread

    nk_aspace_t * old_aspace = get_cur_thread()->aspace;
    nk_aspace_t *mas = nk_aspace_create("paging", "paging_sanity",&c);
    

    if (!mas) {
        nk_vc_printf("failed to create new address space\n");
        test_failed = 1;
        goto no_paging_exit;
    }

    

    nk_aspace_region_t r, r1, r2;
    /**
     * create a 1-1 region mapping all of physical memory
     * so that the kernel can work when that thread is active
     **/
    r.va_start = 0;
    r.pa_start = 0;
    r.len_bytes = LEN_4GB;  // first 4 GB are mapped
    
    /**
     * set protections for kernel
     * use EAGER to tell paging implementation that it needs to build all these PTs right now
     **/ 
    r.protect.flags = NK_ASPACE_READ | NK_ASPACE_WRITE | NK_ASPACE_EXEC | NK_ASPACE_PIN | NK_ASPACE_KERN | NK_ASPACE_EAGER;

    /**
     * now add the region
     * this should build the page tables immediately
     **/
    if (nk_aspace_add_region(mas,&r)) {
        nk_vc_printf("failed to add initial eager region to address space\n");
        test_failed = 1;
        goto clean_up;
    }

    /**
     *  Add another region 
     *  Expect success, VA is not overlapping
     **/
    r1.va_start = ADDR_4GB;
    r1.pa_start = 0;
    r1.len_bytes = LEN_4GB;  // first 4 GB are mapped
    
    r1.protect.flags = NK_ASPACE_READ | NK_ASPACE_WRITE | NK_ASPACE_EXEC | NK_ASPACE_PIN | NK_ASPACE_KERN | NK_ASPACE_EAGER;

    if (nk_aspace_add_region(mas,&r1)) {
        nk_vc_printf("failed to add initial eager region to address space\n");
        test_failed = 1;
        goto clean_up;
    }


    /**
     *  now we will remap the kernel starting at the following address
     *  this is the start of the "canonical upper half", which is
     *  where pre-meltown/spectre kernels used to place themselves
     **/
    r2.va_start = ADDR_UPPER;
    r2.pa_start = 0;
    r2.len_bytes = LEN_4GB;  // first 4 GB are mapped
    r2.protect.flags = NK_ASPACE_READ | NK_ASPACE_WRITE | NK_ASPACE_EXEC | NK_ASPACE_PIN | NK_ASPACE_KERN;

    /**
     *  This one is lazily implemented
     *  Expect success, VA is not overlapping
     **/
    if (nk_aspace_add_region(mas,&r2)) {
        nk_vc_printf("failed to add secondary lazy region to address space\n");
        test_failed = 1;
        goto clean_up;
    }



    /**
     *  Test overlapping detection
     *  This targets to test the overlap check works  
     *  We add series of region {(VA = r2.va_start + r2.len_bytes + 512K * 2i, PA = 0, len_bytes = 512K) | 512K * 2i < 16M}
     *  In human language, we add region starting from r2.va_start + r2.len_bytes.
     *      Each region has length of 512K, and each of them is also gapped by 512K.
     *      We add the regions until the start of the region is 16M beyond r2.va_start + r2.len_bytes.
     *      All of the regions are mapped to PA = 0
     **/

    /**
     *   Every 8 slots represent 512K, xxx means allocated, --- means not allocated
     *   xxxxxxxx--------xxxxxxxx--------xxxxxxxx--------xxxxxxxx--------xxxxxxxx--------xxxxxxxx--------
     *  |               |                                               |
     *  End of r2       1MB after r2                                    4MB after r2
     * */

    nk_aspace_region_t reg_it, reg_overlap;
    reg_it.pa_start = 0;
    reg_it.len_bytes = LEN_512KB;  // 512K
    reg_it.protect.flags = NK_ASPACE_READ | NK_ASPACE_WRITE | NK_ASPACE_EXEC | NK_ASPACE_PIN | NK_ASPACE_KERN;
    uint64_t offset = 0;

    for (offset = 0; offset < LEN_16MB; offset += 2 * reg_it.len_bytes) {
        reg_it.va_start = r2.va_start + r2.len_bytes + offset;
        if (nk_aspace_add_region(mas,&reg_it)) {
            nk_vc_printf("failed to add overlapped region to address space\n");
            test_failed = 1;
            goto clean_up;
        }
    }


    /**
     *  Try to add a region that overlaps with one of the region just added
     * 
     *   Every 8 slots represent 512K, xxx means allocated, --- means not allocated
     *   xxxxxxxx--------xxxxxxxx--------xxxxxxxx--------xxxxxxxx--------xxxxxxxx--------xxxxxxxx--------
     *  |                   |                                           |
     *  End of r2       Try insert here                                4MB after r2
     *                       xxxx
     * */
    reg_overlap.va_start = r2.va_start + r2.len_bytes + 5 * reg_it.len_bytes/2;
    reg_overlap.pa_start = 0;
    reg_overlap.len_bytes = LEN_256KB;  // 256K
    reg_overlap.protect.flags = NK_ASPACE_READ | NK_ASPACE_WRITE | NK_ASPACE_EXEC | NK_ASPACE_PIN | NK_ASPACE_KERN;

    if (!nk_aspace_add_region(mas,&reg_overlap)) {
        nk_vc_printf("Failed to Detect overlapped region to address space" REGION_FORMAT "!\n", REGION(&reg_overlap));
        test_failed = 1;
        goto clean_up;
    }

    /**
     *  Try to add another region with overlapping
     * 
     *    Every 8 slots represent 512K, xxx means allocated, --- means not allocated
     *   xxxxxxxx--------xxxxxxxx--------xxxxxxxx--------xxxxxxxx--------xxxxxxxx--------xxxxxxxx--------
     *          |                                                       |
     *         Try insert here                                          4MB after r2
     *           xxxxxxxxxxxx
     **/
    reg_overlap.va_start = r2.va_start + r2.len_bytes + reg_it.len_bytes;
    reg_overlap.len_bytes = LEN_512KB + LEN_256KB;  

    if (!nk_aspace_add_region(mas,&reg_overlap)) {
        nk_vc_printf("Failed to Detect overlapped region to address space" REGION_FORMAT "!\n", REGION(&reg_overlap));
        test_failed = 1;
         goto clean_up;
    }

    nk_vc_printf("    Survived Region overlapping test\n");




    if (nk_aspace_move_thread(mas)) {
        nk_vc_printf("failed to move shell thread to new address space\n");
        test_failed = 1;
        goto clean_up;
    }

    /**
     *  set CR0.WP (write protect)
     *  For purpose of testing write protection
     * */
    write_cr0(read_cr0() | (1<<16));

    nk_vc_printf("    Survived: moving thread into paging space at %p\n", mas);
    
    
    
    

    /**
     *  start reading the kernel from address 0xffff80000.....+ 1 MB
     *  Compare eagerly drilled region and lazily drilled region for 4MB
     *   also, this will fault in pages as we go, expect page fault handled by paging
     * */
    if (memcmp(r.va_start , r2.va_start , LEN_4MB)) {
	    nk_vc_printf("Weird, low-mapped and high-mapped differ...\n");
        nk_vc_printf("Weird, low-mapped = %lx and high-mapped = %lx\n", r.va_start, r2.va_start);
        test_failed = 1;
        goto clean_up;
    } 	

    nk_vc_printf("    Survived: memory comparison of one eager and one lazy copy\n");

    /**
     *  Compare two eagerly drilled region for first 4MB
     * */
    if (memcmp(r.va_start, r1.va_start, LEN_4MB)) {
        nk_vc_printf("Weird, two early added region differ...\n");
        test_failed = 1;
        goto clean_up;
    } 	

    nk_vc_printf("    Survived: memory comparison of two eager mapped copies\n");


    
    
    /**
     *  try to access region not defined, should panic, if uncommented the block below
     * */
    // if (memcmp(r.va_start, r2.va_start + 2 * r2.len_bytes , LEN_1KB)) {
    //     test_failed = 1;
    //     nk_vc_printf("should fail\n");
    //     goto clean_up;
    // }






    /**
     *  test case for move region 
     *  initially, r3 (8G -> 8G), r4 = (12G -> 8G), r5  = (12G -> 0)
     *  call move_region(apsace, r4, r5)
     *      should expect 12G address points to 0
     * */

    nk_aspace_region_t r3, r4, r5;

    r3.va_start = ADDR_8GB;
    r3.pa_start = ADDR_8GB;
    r3.len_bytes = LEN_4GB; 
    r3.protect.flags = NK_ASPACE_READ | NK_ASPACE_WRITE | NK_ASPACE_EXEC | NK_ASPACE_PIN | NK_ASPACE_KERN | NK_ASPACE_EAGER;

    if (nk_aspace_add_region(mas,&r3)) {
        test_failed = 1;
        nk_vc_printf("failed to add eager region r3"
                    "(va=%016lx pa=%016lx len=%lx, prot=%lx)" 
                    "to address space\n",
                    r3.va_start, r3.pa_start, r3.len_bytes, r3.protect.flags    
        );
        goto clean_up;
    }

    

    r4.va_start = ADDR_12GB;
    r4.pa_start = ADDR_8GB;
    r4.len_bytes = LEN_4GB;
    r4.protect.flags = NK_ASPACE_READ | NK_ASPACE_WRITE | NK_ASPACE_EXEC | NK_ASPACE_KERN | NK_ASPACE_EAGER;

    if (nk_aspace_add_region(mas,&r4)) {
        test_failed = 1;
        nk_vc_printf("failed to add eager region r4"
                    "(va=%016lx pa=%016lx len=%lx, prot=%lx)" 
                    "to address space\n",
                    r4.va_start, r4.pa_start, r4.len_bytes, r4.protect.flags    
        );
        goto clean_up;
    }


    /**
     *  Initially, r3 and r4 should share samne content
     *  VA = 8G points to PA = 8G, but VA = 12G points to PA = 8G
     * */
    if (memcmp(r3.va_start, r4.va_start, LEN_1MB)) {
	    nk_vc_printf("Weird, r3 and r4  differ...\n");
    }

    
    // nk_vc_printf("    Survived: memory comparison of r3 and r4\n");


    r5.va_start = (void*) r4.va_start;
    r5.pa_start = (void*) 0;
    r5.len_bytes = r4.len_bytes;
    r5.protect.flags = r4.protect.flags;

    if(nk_aspace_move_region(mas, &r4, &r5)) {
        test_failed = 1;
        nk_vc_printf("failed to move region r4 to r5\n");
        goto clean_up;
    }

    /**
     *  After the move, VA = 12G points to PA = 0
     **/
    if (memcmp((void*) r.va_start , (void*) r4.va_start, LEN_4MB)) {
        test_failed = 1;
	    nk_vc_printf("Weird, r and r5  differ...\n");
        goto clean_up;
    }
    
    // nk_vc_printf("    Survived: memory comparison of r and r5\n");
    

    /**
     *  Right now, r3 and r4 should be different
     *  VA = 8G points to PA = 8G, but VA = 12G points to PA = 0
     **/
    if (!memcmp(r3.va_start, r4.va_start, LEN_4MB)) {
        test_failed = 1;
	    nk_vc_printf("Weird, r3 and r4 should differ\n");
        goto clean_up;
    }

    nk_vc_printf("    Survived: move region test\n");






    /**
     *  test case for remove region
     *      Before removal of r5. Add another region with SAME VA definition should fail.
     *      After  removal of r5. Add another region with SAME VA should suceed.
     * */
    nk_aspace_region_t r5_copy = r5;

    if (!nk_aspace_add_region(mas, &r5_copy)) {
        test_failed = 1;
        nk_vc_printf("Failed to detect exact copy region overlapping"
                    "(va=%016lx pa=%016lx len=%lx, prot=%lx)" 
                    "to address space\n",
                    r5_copy.va_start, r5_copy.pa_start, r5_copy.len_bytes, r5_copy.protect.flags    
        );
        goto clean_up;
    }
     

    /**
     *  reference r5 should be successful
     * */
    if (memcmp((void*) r5.va_start , (void*) r5.va_start, LEN_1MB)) {
	    nk_vc_printf("Reference r5 at %16lx FAIL\n", r5.va_start);
    }

    if (nk_aspace_remove_region(mas,&r5)) {
        test_failed = 1;
        nk_vc_printf("failed to remove eager region r5"
                    "(va=%016lx pa=%016lx len=%lx, prot=%lx)" 
                    "to address space\n",
                    r5.va_start, r5.pa_start, r5.len_bytes, r5.protect.flags    
        );
        goto clean_up;
    }

    // should fail
    // if (memcmp((void*) r5.va_start , (void*) r5.va_start, LEN_1MB)) {
    //     nk_vc_printf("Reference r5 at %16lx FAIL\n", r5.va_start);
    // }

    /**
     *  Add region should be successful here
     * */
    if (nk_aspace_add_region(mas, &r5_copy)) {
        test_failed = 1;
        nk_vc_printf("Failed to add copy region"
                    "(va=%016lx pa=%016lx len=%lx, prot=%lx)" 
                    "to address space\n",
                    r5_copy.va_start, r5_copy.pa_start, r5_copy.len_bytes, r5_copy.protect.flags    
        );
        goto clean_up;
    }

    /**
     *  reference r5_copy should be successful
     * */
    if (memcmp((void*) r5_copy.va_start , (void*) r5_copy.va_start, LEN_1MB)) {
        nk_vc_printf("Reference r5_copy at %16lx FAIL\n", r5_copy.va_start);
    }
    
    nk_vc_printf("    Survived: removal region test\n");





    /**
     *  Test case for protection region
     *      1. we test that a pinned region cannot be moved/removed
     *      2. we test that a we can write to a region after updating its write access
     * */

    nk_aspace_region_t reg;
    reg.va_start = ADDR_16GB; 
    reg.pa_start = ADDR_1GB;
    reg.len_bytes = LEN_6MB;  
    reg.protect.flags = NK_ASPACE_READ  | NK_ASPACE_EXEC | NK_ASPACE_PIN | NK_ASPACE_KERN ;
    //  reg.protect.flags =  NK_ASPACE_WRITE | NK_ASPACE_EXEC | NK_ASPACE_PIN | NK_ASPACE_KERN | NK_ASPACE_EAGER;

    if (nk_aspace_add_region(mas, &reg)) {
        test_failed = 1;
        nk_vc_printf("failed to add eager region reg"
                    "(va=%016lx pa=%016lx len=%lx, prot=%lx)" 
                    "to address space\n",
                    reg.va_start, reg.pa_start, reg.len_bytes, reg.protect.flags    
        );
	    goto clean_up;
    }
    
    /**
     *  Try to remove pinned region, should fail
     * */
    if (!nk_aspace_remove_region(mas,&reg)) {
        test_failed = 1;
        nk_vc_printf("ERROR: remove pinned region!\n");
        goto clean_up;
    } 


    /**
     *  Try to move pinned region, should fail
     *      Dummy move though, a region move to itself.
     * */
    if (!nk_aspace_move_region(mas,&reg, &reg)) {
        test_failed = 1;
        nk_vc_printf("ERROR: move pinned region!\n");
        goto clean_up;
    } 
    
    
    /**
     *  Expect to crash if uncomment the following line
     **/
    // memcpy((void*)(reg.va_start),(void*)0x0, LEN_1KB);

    /**
     *  Update region's protection with write access
     * */
    nk_aspace_protection_t prot;
    // prot.flags = NK_ASPACE_READ  | NK_ASPACE_WRITE | NK_ASPACE_EXEC | NK_ASPACE_KERN | NK_ASPACE_EAGER;
    prot.flags = NK_ASPACE_READ  | NK_ASPACE_WRITE | NK_ASPACE_EXEC | NK_ASPACE_KERN ;
    nk_aspace_protect_region(mas, &reg, &prot);
    reg.protect = prot;

    memcpy((void*)(reg.va_start), (void*)0x0, LEN_1KB);


    nk_vc_printf("    Survived: Protection test\n");



clean_up:

    if(nk_aspace_move_thread(old_aspace) ) {
        nk_vc_printf("Failed move thread from %p to %p\n", mas, old_aspace);
        test_failed = 1;
    }

    nk_vc_printf("    Survived: Move thread back to old_aspace at %p\n", old_aspace);

    if(nk_aspace_destroy(mas)){
        nk_vc_printf("Something wrong during destorying the new aspace\n");
        test_failed = 1;
    } else {
        nk_vc_printf("Destory succeeded\n");
    }

    if(test_failed){
        nk_vc_printf("Paging check sanity test Failed!\n");
    } else {
        nk_vc_printf("Paging check sanity test Passed!\n");
    }

    return 0;

no_paging_exit:
    nk_vc_printf("Paging NOT created or Failed to create paging!\n");
    return 0;
}


static struct shell_cmd_impl paging_sanity_check = {
    .cmd      = "testPaging",
    .help_str = "Sanity Check for Paging",
    .handler  = paging_sanity,
};

nk_register_shell_cmd(paging_sanity_check);