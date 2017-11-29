/*
 *
 * device driver for TW6869 based PCIe capture cards
 * driver core for hardware
 *
 */

#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kmod.h>
#include <linux/sound.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/dma-mapping.h>
#include <linux/pm.h>
#include <linux/vmalloc.h>

#include "TW68.h"
#include "TW68_defines.h"

MODULE_DESCRIPTION
    ("v4l2 driver module for TW6868/6869 based CVBS video capture cards");
MODULE_AUTHOR("Simon Xu 2011-2013 @intersil");
MODULE_LICENSE("GPL");

/* ------------------------------------------------------------------ */

static unsigned int irq_debug;
module_param(irq_debug, int, 0644);
MODULE_PARM_DESC(irq_debug, "enable debug messages [IRQ handler]");

static unsigned int core_debug;
module_param(core_debug, int, 0644);
MODULE_PARM_DESC(core_debug, "enable debug messages [core]");

static unsigned int gpio_tracking;
module_param(gpio_tracking, int, 0644);
MODULE_PARM_DESC(gpio_tracking, "enable debug messages [gpio]");

static unsigned int alsa = 1;
module_param(alsa, int, 0644);
MODULE_PARM_DESC(alsa, "enable/disable ALSA DMA sound [dmasound]");

static unsigned int latency = UNSET;
module_param(latency, int, 0444);
MODULE_PARM_DESC(latency, "pci latency timer");

static unsigned int video_nr[] = {[0 ... (TW68_MAXBOARDS - 1)] = UNSET };
static unsigned int vbi_nr[] = {[0 ... (TW68_MAXBOARDS - 1)] = UNSET };
static unsigned int radio_nr[] = {[0 ... (TW68_MAXBOARDS - 1)] = UNSET };
static unsigned int tuner[] = {[0 ... (TW68_MAXBOARDS - 1)] = UNSET };
static unsigned int card[] = {[0 ... (TW68_MAXBOARDS - 1)] = UNSET };

module_param_array(video_nr, int, NULL, 0444);
module_param_array(vbi_nr, int, NULL, 0444);
module_param_array(radio_nr, int, NULL, 0444);
module_param_array(tuner, int, NULL, 0444);
module_param_array(card, int, NULL, 0444);

MODULE_PARM_DESC(video_nr, "video device number");
MODULE_PARM_DESC(vbi_nr, "vbi device number");
MODULE_PARM_DESC(radio_nr, "radio device number");
MODULE_PARM_DESC(tuner, "tuner type");
MODULE_PARM_DESC(card, "card type");

DEFINE_MUTEX(TW686v_devlist_lock);
EXPORT_SYMBOL(TW686v_devlist_lock);
LIST_HEAD(TW686v_devlist);
EXPORT_SYMBOL(TW686v_devlist);

#define container_of(ptr, type, member) \
	({  const typeof( ((type *)0)->member ) *__mptr = (ptr); \
	(type *)( (char *)__mptr - offsetof(type,member) );})

/* ------------------------------------------------------------------ */

static char name_comp1[] = "Composite1";
static char name_comp2[] = "Composite2";
static char name_comp3[] = "Composite3";
static char name_comp4[] = "Composite4";

/* ------------------------------------------------------------------ */
/* board config info                                                  */

static DEFINE_MUTEX(TW68_devlist_lock);

struct TW68_board TW68_boards[] = {
	[TW68_BOARD_UNKNOWN] = {
		.name = "TW6869",
		.audio_clock = 0,
		.tuner_type = TUNER_ABSENT,
		.radio_type = UNSET,
		.tuner_addr = ADDR_UNSET,
		.radio_addr = ADDR_UNSET,
		.inputs = {{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE1,
		}},
	},

	[TW68_BOARD_A] = {
		.name = "TW6869",
		.audio_clock = 0,
		.tuner_type = TUNER_ABSENT,
		.radio_type = UNSET,
		.tuner_addr = ADDR_UNSET,
		.radio_addr = ADDR_UNSET,

		.inputs = {{
			.name = name_comp1,
			.vmux = 0,
			.amux = LINE1,
		}, {
			.name = name_comp2,
			.vmux = 1,
			.amux = LINE2,
		}, {
			.name = name_comp3,
			.vmux = 2,
			.amux = LINE3,
		}, {
			.name = name_comp4,
			.vmux = 4,
			.amux = LINE4,
		}},
	},
};

const unsigned int TW68_bcount = ARRAY_SIZE(TW68_boards);

/* ------------------------------------------------------------------ */
/* PCI ids + subsystem IDs                                            */

struct pci_device_id TW68_pci_tbl[] = {
	{
	 .vendor = 0x1797,
	 .device = 0x6869,
	 .subvendor = 0,
	 .subdevice = 0,
	 .driver_data = 0,
	}, { }
};

MODULE_DEVICE_TABLE(pci, TW68_pci_tbl);
/* ------------------------------------------------------------------ */

static LIST_HEAD(mops_list);
static unsigned int TW68_devcount;

int (*TW68_dmasound_init) (struct TW68_dev * dev);
int (*TW68_dmasound_exit) (struct TW68_dev * dev);

#define dprintk(fmt, arg...)	if (core_debug) \
	printk(KERN_DEBUG "%s/core: " fmt, dev->name , ## arg)

/* ------------------------------------------------------------------ */
/*
void dma_field_init(struct dma_region *dma)
{
	dma->kvirt = NULL;
	dma->dev = NULL;
	dma->n_pages = 0;
	dma->n_dma_pages = 0;
	dma->sglist = NULL;
}
*/
/**
 * dma_region_free - unmap and free the buffer
 */
void dma_field_free(struct dma_region *dma)
{
	if (dma->n_dma_pages) {
		pci_unmap_sg(dma->dev, dma->sglist, dma->n_pages,
			     dma->direction);
		dma->n_dma_pages = 0;
		dma->dev = NULL;
	}

	vfree(dma->sglist);
	dma->sglist = NULL;

	vfree(dma->kvirt);
	dma->kvirt = NULL;
	dma->n_pages = 0;
}

/**   struct pci_dev *pci,
 * dma_region_alloc - allocate a buffer and map it to the IOMMU
 */
int dma_field_alloc(struct dma_region *dma, unsigned long n_bytes,
		    struct pci_dev *dev, int direction)
{
	unsigned int i;

	/* round up to page size */
	/*  to align the pointer to the (next) page boundary
	   #define PAGE_ALIGN(addr)        (((addr) + PAGE_SIZE - 1) & PAGE_MASK)
	   this worked as PAGE_SIZE and PAGE_MASK were available in page.h.
	 */

	n_bytes = PAGE_ALIGN(n_bytes);

	dma->n_pages = n_bytes >> PAGE_SHIFT;

	dma->kvirt = vmalloc_32(n_bytes);
	if (!dma->kvirt) {
		goto err;
	}

	memset(dma->kvirt, 0, n_bytes);

	/* allocate scatter/gather list */
	dma->sglist = vmalloc(dma->n_pages * sizeof(*dma->sglist));
	if (!dma->sglist) {
		goto err;
	}

	sg_init_table(dma->sglist, dma->n_pages);

	/* fill scatter/gather list with pages */
	for (i = 0; i < dma->n_pages; i++) {
		unsigned long va =
		    (unsigned long)dma->kvirt + (i << PAGE_SHIFT);

		sg_set_page(&dma->sglist[i], vmalloc_to_page((void *)va),
			    PAGE_SIZE, 0);
	}

	/* map sglist to the IOMMU */
	dma->n_dma_pages =
	    pci_map_sg(dev, dma->sglist, dma->n_pages, direction);

	if (dma->n_dma_pages == 0) {
		goto err;
	}

	dma->dev = dev;
	dma->direction = direction;

	return 0;

err:
	dma_field_free(dma);
	return -ENOMEM;
}

int TW68_buffer_pages(int size)
{
	size = PAGE_ALIGN(size);
	size += PAGE_SIZE;	/* for non-page-aligned buffers */
	size /= 4096;
	return size;
}

/* calc max # of buffers from size (must not exceed the 4MB virtual
 * address space per DMA channel) 
 */
#if 0 
int TW68_buffer_count(unsigned int size, unsigned int count)
{
	unsigned int maxcount;

	maxcount = 1024 / TW68_buffer_pages(size);
	if (count > maxcount)
		count = maxcount;

	return count;
}
#endif
int TW68_buffer_startpage(struct TW68_buf *buf)
{
	unsigned long pages, n, pgn;
	pages = TW68_buffer_pages(buf->vb.bsize);
	n = buf->vb.i;
	pgn = pages * n;
	return pgn;
}
#if 0
unsigned long TW68_buffer_base(struct TW68_buf *buf)
{
	unsigned long base0, base;
	struct videobuf_dmabuf *dma = videobuf_to_dma(&buf->vb);

	base0 = TW68_buffer_startpage(buf) * 4096;
	base = base0 + dma->sglist[0].offset;

	return base;
}
#endif
/* ------------------------------------------------------------------ */

int AudioDMA_PB_alloc(struct pci_dev *pci, struct TW68_pgtable *pt)
{
	dma_addr_t dma_addr;
	int audio_ch;
	__le32 *pdmaP, *pdmaB;
	__le32 *AudioPages;

	// for audio  CH  P/B

	AudioPages =
	    pci_alloc_consistent(pci, PAGE_SIZE * MAX_NUM_DATA_DMA * 2,
				 &dma_addr);

	if (NULL == AudioPages) {
		return -ENOMEM;
	}

	pt->size = PAGE_SIZE * MAX_NUM_DATA_DMA * 2;	///pt;  //2
	pt->cpu = AudioPages;
	pt->dma = dma_addr;

	for (audio_ch = 0; audio_ch < MAX_NUM_DATA_DMA; audio_ch++) {
		pdmaP = pt->cpu + (PAGE_SIZE << 1) * audio_ch / 4 + 100;
		pdmaB =
		    pt->cpu + ((PAGE_SIZE << 1) * audio_ch + PAGE_SIZE) / 4 +
		    100;
	}
	return 0;
}

/* Allocate memory for descriptor page table */
int videoDMA_pgtable_alloc(struct pci_dev *pci, struct TW68_pgtable *pt)
{
	__le32 *cpu;
	dma_addr_t dma_addr;

	cpu = pci_alloc_consistent(pci, PAGE_SIZE * 8, &dma_addr);

	if (NULL == cpu) {
		return -ENOMEM;
	}

	pt->size = PAGE_SIZE * 8;
	pt->cpu = cpu;
	pt->dma = dma_addr;

	return 0;
}

void TW68_pgtable_free(struct pci_dev *pci, struct TW68_pgtable *pt)
{
	if (NULL == pt->cpu)
		return;
	pci_free_consistent(pci, pt->size, pt->cpu, pt->dma);
	pt->cpu = NULL;
}

/* ------------------------------------------------------------------ */

int TW68_buffer_queue(struct TW68_dev *dev,
		      struct TW68_dmaqueue *q, struct TW68_buf *buf)
{
	if (NULL == q->curr) {
		q->curr = buf;
		buf->activate(dev, buf, NULL);
	} else {
		list_add_tail(&buf->vb.queue, &q->queued);	// curr
		buf->vb.state = VIDEOBUF_QUEUED;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/*
 * Buffer handling routines
 *
 * These routines are "generic", i.e. are intended to be used by more
 * than one module, e.g. the video and the transport stream modules.
 * To accomplish this generality, callbacks are used whenever some
 * module-specific test or action is required.
 */
#if 0
/* resends a current buffer in queue after resume */
int TW68_buffer_requeue(struct TW68_dev *dev, struct TW68_dmaqueue *q)
{
	struct TW68_buf *buf, *prev;

	if (!list_empty(&q->active)) {
		buf = list_entry(q->active.next, struct TW68_buf, vb.queue);
		mod_timer(&q->timeout, jiffies + BUFFER_TIMEOUT);
		return 0;
	}

	prev = NULL;
	for (;;) {
		if (list_empty(&q->queued))
			return 0;
		buf = list_entry(q->queued.next, struct TW68_buf, vb.queue);
		/* if nothing precedes this one */
		if (NULL == prev) {
			list_move_tail(&buf->vb.queue, &q->active);
			buf->activate(dev, buf, NULL);
			dprintk("%s: [%p/%d] first active\n",
			       __func__, buf, buf->vb.i);
		} else {
			list_move_tail(&buf->vb.queue, &q->active);
			buf->activate(dev, buf, NULL);
		}

		return 0;

	}
}
#endif 
void TW68_buffer_finish(struct TW68_dev *dev,
			struct TW68_dmaqueue *q, unsigned int state)
{

	if (q->dev != dev)
		return;

	q->curr->vb.state = state;
	do_gettimeofday(&q->curr->vb.ts);

	wake_up(&q->curr->vb.done);
	q->curr = NULL;
}

void TW68_buffer_next(struct TW68_dev *dev, struct TW68_dmaqueue *q)
{
	struct TW68_buf *buf, *next = NULL;
	BUG_ON(NULL != q->curr);

	if (!list_empty(&q->queued)) {
		/* activate next one from  dma queue */
		buf = list_entry(q->queued.next, struct TW68_buf, vb.queue);

		list_del(&buf->vb.queue);

		if (!list_empty(&q->queued))
			next =
			    list_entry(q->queued.next, struct TW68_buf,
				       vb.queue);
		q->curr = buf;

		buf->vb.state = VIDEOBUF_ACTIVE;

		mod_timer(&q->timeout, jiffies + BUFFER_TIMEOUT);
	} else {
		/* nothing to do -- just stop DMA */
		del_timer(&q->timeout);
	}
}
#if 0
void Field_SG_Mapping(struct TW68_dev *dev, int field_PB)	//    0 1
{
	struct TW68_dmaqueue *q;
	struct TW68_buf *buf;
	struct TW68_pgtable *pt;
	unsigned int i, nbytes, FieldSize, remain, nIDX, pgn;
	u32 dwCtrl;
	__le32 *ptr;
	u32 nId = 0;
	u32 m_CurrentFrameStartIdx = 0;	//128 *nId;
	u32 m_NextFrameStartIdx = 0;	//128 *nId;
	struct videobuf_dmabuf *dma;
	struct scatterlist *list;

	q = &dev->video_q;

	if (!list_empty(&q->queued)) {
		/* get next buffer from  dma queue */
		buf = list_entry(q->queued.next, struct TW68_buf, vb.queue);

		// fill half frame SG mapping entries
		dma = videobuf_to_dma(&buf->vb);
		list = dma->sglist;
		// dma channel offset = 8192 /8 /4 /2;
		ptr = dev->m_Page0.cpu + (2 * 128 * nId);	// channel start entry address

		pt = &dev->m_Page0;
		BUG_ON(NULL == pt || NULL == pt->cpu);

		nIDX = 128;	///85;
		pgn = 83;
		// dma channel offset = 8192 /8 /4 /2;
		ptr = pt->cpu + (2 * nIDX * nId);	// channel start entry address   128

		FieldSize = buf->vb.size / 2;

		nbytes = 0;
		for (i = 0; i < dma->sglen; i++, list++)
		{
			// switch to B
			if (((nbytes + list->length) >= FieldSize)
			    && ((nbytes + list->length) <=
				(FieldSize + list->length))) {

				remain = FieldSize - (nbytes);
				if (remain > 0) {

					dwCtrl =
					    (((DMA_STATUS_HOST_READY & 0x3) << 30) |
					     ((m_CurrentFrameStartIdx & 0xFF) << 14) |
					     ((m_NextFrameStartIdx & 0xFF) << 21) |
					     (remain & 0x1FFF));	// size

					if (field_PB)
						ptr++;
					else
						*(ptr++) = cpu_to_le32(dwCtrl);

					if (field_PB)
						ptr++;
					else
						*(ptr++) = cpu_to_le32(sg_dma_address(list) - list->offset);	/// setup page dma address

				}

				remain = (nbytes + list->length) - FieldSize;
				nbytes += list->length;

				ptr = (pt->cpu + (2 * nIDX * nId)) + 0x800;	// 2 pages distance

				dwCtrl =
				    (((DMA_STATUS_HOST_READY & 0x3) << 30) |
				     (((1) & 1) << 29) |
				     ((m_CurrentFrameStartIdx & 0xFF) << 14) |
				     ((m_NextFrameStartIdx & 0xFF) << 21) |
				     (remain & 0x1FFF));	// size

				if (field_PB)
					*(ptr++) = cpu_to_le32(dwCtrl);
				else
					ptr++;

				if (field_PB)
					*(ptr++) = cpu_to_le32(sg_dma_address(list) - list->offset + list->length - remain);	/// setup page dma address
				else
					ptr++;
			}

			else {

				dwCtrl = (((DMA_STATUS_HOST_READY & 0x3) << 30)
						| (((i == 0) & 1) << 29) |
						((m_CurrentFrameStartIdx & 0x7F) << 14) | 
						((m_NextFrameStartIdx & 0xFF) << 21) | 
						(list->length & 0x1FFF));	// size

				if (((!field_PB)
				     && ((nbytes + list->length) <= FieldSize))
				    || ((field_PB) && (nbytes >= FieldSize)))
					*(ptr++) = cpu_to_le32(dwCtrl);
				else
					ptr++;	/// pointing to the Address dword

				if (((!field_PB)
				     && ((nbytes + list->length) <= FieldSize))
				    || ((field_PB) && (nbytes >= FieldSize)))
					*(ptr++) = cpu_to_le32(sg_dma_address(list)
							- list->offset);	/// setup page dma address
				else
					ptr++;	/// pointing to the Address dword

				nbytes += list->length;
			}
		}
	}
	/* else: nothing to do -- just stop DMA */
}

void Fixed_SG_Mapping(struct TW68_dev *dev, int nDMA_channel, int Frame_size)	//    0 1
{
	// PROGRAM DMA DESCRIPTOR WITH FIXED vma dma_region sglist
	struct dma_region *Field_P;
	struct dma_region *Field_B;
	struct scatterlist *sglist;
	struct TW68_pgtable *pt;
	int i, nbytes, FieldSize, remain, nIDX, pgn, pgn0;	// unsigned xxx
	u32 dwCtrl;
	u32 m_CurrentFrameStartIdx = 0;
	u32 m_NextFrameStartIdx = 0;

	__le32 *ptr;
	u32 nId = nDMA_channel;

	// fill P field half frame SG mapping entries
	Field_P = &dev->Field_P[nId];
	Field_B = &dev->Field_B[nId];

	pt = &dev->m_Page0;
	BUG_ON(NULL == pt || NULL == pt->cpu);
	FieldSize = Frame_size / 2;

	nIDX = 128;		///85;
	//// pgn = 85;   /// FieldSize / 4096;
	pgn0 = (FieldSize + 4095) / 4096;
	pgn = TW68_buffer_pages(Frame_size / 2) - 1;	// page number for 1 field

	m_NextFrameStartIdx += pgn;
	ptr = pt->cpu + (2 * nIDX * nId);
	printk
	    ("-??????????????????-_pgtable--nId %d,  pgn0 %d   pgn %d    m_NextFrameStartIdx: %d \n ",
	     nId, pgn0, pgn, m_NextFrameStartIdx);

	sglist = Field_P->sglist;
	nbytes = 0;
	remain = 0;
	for (i = 0; i < Field_P->n_pages; i++, sglist++) {
		// switch to B
		if ((nbytes + sglist->length) <= FieldSize) {
			remain = sglist->length;
		} else {
			remain = FieldSize - nbytes;
		}

		if (remain <= 0)
			break;

		{

			dwCtrl = (((DMA_STATUS_HOST_READY & 0x3) << 30) |
				  (((i == 0) & 1) << 29) |
				  ((m_CurrentFrameStartIdx & 0x7F) << 14) | 
				  ((m_NextFrameStartIdx & 0xFF) << 21) | 
				  ((pgn > 70) << 13) |	///   70
				  (remain & 0x1FFF));	// size

			*(ptr++) = cpu_to_le32(dwCtrl);

			*(ptr++) = cpu_to_le32(sg_dma_address(sglist) -
					sglist->offset);	/// setup page dma address

			nbytes += sglist->length;
		}
	}

	remain = 0;		/// (nbytes + sglist->length) - FieldSize;
	nbytes = 0;
	sglist = Field_B->sglist;

	ptr = (pt->cpu + (2 * nIDX * nId)) + 0x800;	// 2 pages distance  switch to B

	for (i = 0; i < Field_B->n_pages; i++, sglist++) {
		// switch to B
		if ((nbytes + sglist->length) <= FieldSize) {
			remain = sglist->length;
		} else {
			remain = FieldSize - nbytes;
		}

		if (remain <= 0)
			break;

		dwCtrl = (((DMA_STATUS_HOST_READY & 0x3) << 30) |
			  (((i == 0) & 1) << 29) |
			  ((m_CurrentFrameStartIdx & 0x7F) << 14) |	/// 0xFF
			  ((m_NextFrameStartIdx & 0xFF) << 21)
				| ((pgn > 70) << 13) | (remain & 0x1FFF));  // size

		*(ptr++) = cpu_to_le32(dwCtrl);
		/// setup page dma address
		*(ptr++) = cpu_to_le32(sg_dma_address(sglist) - sglist->offset);
		nbytes += sglist->length;	/// remain
	}

}
#endif
void BFDMA_setup(struct TW68_dev *dev, int nDMA_channel, int H, int W)	//    Field0   P B    Field1  P B     WidthHightPitch
{
	u32 regDW, dwV, dn;

	reg_writel((BDMA_ADDR_P_0 + nDMA_channel * 8),
			dev->BDbuf[nDMA_channel][0].dma_addr);	//P DMA page table
	reg_writel((BDMA_ADDR_B_0 + nDMA_channel * 8),
		   dev->BDbuf[nDMA_channel][1].dma_addr);
	reg_writel((BDMA_WHP_0 + nDMA_channel * 8),
		   (W & 0x7FF) | ((W & 0x7FF) << 11) | ((H & 0x3FF) << 22));

	reg_writel((BDMA_ADDR_P_F2_0 + nDMA_channel * 8),
			dev->BDbuf[nDMA_channel][2].dma_addr);	//P DMA page table
	reg_writel((BDMA_ADDR_B_F2_0 + nDMA_channel * 8),
		   dev->BDbuf[nDMA_channel][3].dma_addr);
	reg_writel((BDMA_WHP_F2_0 + nDMA_channel * 8),
		   (W & 0x7FF) | ((W & 0x7FF) << 11) | ((H & 0x3FF) << 22));

	regDW = reg_readl(PHASE_REF_CONFIG);
	dn = (nDMA_channel << 1) + 0x10;
// GN: USE_FIELD_MODE ?
	dwV = (0x2 << dn);

	regDW |= dwV;
	reg_writel(PHASE_REF_CONFIG, regDW);
	dwV = reg_readl(PHASE_REF_CONFIG);
}
#if 0
int Field_Copy(struct TW68_dev *dev, int nDMA_channel, int field_PB)
{
	struct TW68_dmaqueue *q;
	struct TW68_buf *buf = NULL;
	int Hmax, Wmax, h, pos, pitch;

	struct dma_region *Field_P;
	struct dma_region *Field_B;

	int nId = nDMA_channel + 1;

	void *vbuf, *srcbuf;	// = videobuf_to_vmalloc(&buf->vb);

	// fill P field half frame SG mapping entries
	Field_P = &dev->Field_P[nDMA_channel];
	Field_B = &dev->Field_B[nDMA_channel];

	if (field_PB)
		srcbuf = Field_B->kvirt;
	else
		srcbuf = Field_P->kvirt;

	q = &dev->video_dmaq[nId];	///  &dev->video_q;

	if (q->curr) {
		buf = q->curr;
		vbuf = videobuf_to_vmalloc(&buf->vb);

		Hmax = buf->vb.height / 2;
		Wmax = buf->vb.width;

		pitch = Wmax * buf->fmt->depth / 8;
		pos = pitch * (field_PB);

		for (h = 0; h < Hmax; h++) {
			memcpy(vbuf + pos, srcbuf, pitch);
			pos += pitch * 2;
			srcbuf += pitch;
		}
	} else {
		return 0;
	}
	return 1;
}
#endif
int BF_Copy(struct TW68_dev *dev, int nDMA_channel, u32 Fn, u32 PB)
{
	struct TW68_dmaqueue *q;
	struct TW68_buf *buf = NULL;	//,*next = NULL;
	int n, Hmax, Wmax, pos, pitch;
	int nId = nDMA_channel + 1;

	void *vbuf, *srcbuf;	// = videobuf_to_vmalloc(&buf->vb);


	// fill P field half frame SG mapping entries
	pos = 0;
	n = 0;

	if (Fn)
		n = 2;

	if (PB)
		n++;

	srcbuf = dev->BDbuf[nDMA_channel][n].cpu;

	q = &dev->video_dmaq[nId];	///  &dev->video_q;

	if (q->curr) {
		buf = q->curr;
		vbuf = videobuf_to_vmalloc(&buf->vb);

		Hmax = buf->vb.height / 2;
		Wmax = buf->vb.width;

		pitch = Wmax * buf->fmt->depth / 8;

		if (Fn)
			pos = pitch;

		memcpy(vbuf, srcbuf, Hmax * 2 * pitch);	//Test the top half frame
	} else {
		return 0;
	}
	return 1;
}

int QF_Field_Copy(struct TW68_dev *dev, int nDMA_channel, u32 Fn, u32 PB)
{
	struct TW68_dmaqueue *q;
	struct TW68_buf *buf = NULL;
	int Hmax, Wmax, h, n, pos, pitch, stride;
	int nId = 0;

	void *vbuf, *srcbuf;	// = videobuf_to_vmalloc(&buf->vb);

	n = 0;
	if (Fn)
		n = 2;
	if (PB)
		n++;

	srcbuf = dev->BDbuf[nDMA_channel][n].cpu;

	q = (&dev->video_dmaq[nId]);	///  &dev->video_q; (unsigned long)

	if (q->curr) {
		buf = q->curr;
		Hmax = buf->vb.height / 2;
		Wmax = buf->vb.width / 2;

		pitch = 2 * Wmax * buf->fmt->depth / 8;
		stride = pitch / 2;

		if (nDMA_channel == 0)
			pos = 0;
		if (nDMA_channel == 1)
			pos += stride;
		if (nDMA_channel == 2)
			pos += pitch * Hmax;
		if (nDMA_channel == 3)
			pos += pitch * Hmax + stride;

		vbuf = videobuf_to_vmalloc(&buf->vb);

		for (h = 0; h < Hmax - 0; h++) {
			memcpy(vbuf + pos, srcbuf, stride);
			pos += pitch;
			srcbuf += stride;
		}
	} else {
		return 0;
	}
	return 1;
}

void DecoderResize(struct TW68_dev *dev, int nId, int nHeight, int nWidth)
{
	u32 nAddr, nHW, nH, nW, nVal, nReg, regDW;

	if (nId >= 8) {
		return;
	}

	// only for internal 4     HDelay VDelay   etc
	nReg = 0xe7;		//  blue back color
	reg_writel(MISC_CONTROL2, nReg);

	if (dev->PAL50[nId + 1]) {
		//VDelay
		regDW = 0x18;
		if (nId < 4) {
			nAddr = VDELAY0 + (nId * 0x10);
			reg_writel(nAddr, regDW);

		} else {
			nAddr = VDELAY0 + ((nId - 4) * 0x10) + 0x100;
			reg_writel(nAddr, regDW);
		}

		//HDelay
		regDW = 0x0A;
		regDW = 0x0C;

		if (nId < 4) {
			nAddr = HDELAY0 + (nId * 0x10);
			reg_writel(nAddr, regDW);
		} else {
			nAddr = HDELAY0 + ((nId - 4) * 0x10) + 0x100;
			reg_writel(nAddr, regDW);
		}
	} else {
		//VDelay
		regDW = 0x14;
		if (nId < 4) {
			nAddr = VDELAY0 + (nId * 0x10);
			reg_writel(nAddr, regDW);

		} else {
			nAddr = VDELAY0 + ((nId - 4) * 0x10) + 0x100;
			reg_writel(nAddr, regDW);
		}

		//HDelay
		regDW = 0x0D;
		regDW = 0x0E;

		if (nId < 4) {
			nAddr = HDELAY0 + (nId * 0x10);
			reg_writel(nAddr, regDW);
		} else {
			nAddr = HDELAY0 + ((nId - 4) * 0x10) + 0x100;
			reg_writel(nAddr, regDW);
		}
	}

	nVal = reg_readl(HDELAY0 + nId);

	nHW = nWidth | (nHeight << 16) | (1 << 31);
	nH = nW = nHW;

	//Video Size
	reg_writel(VIDEO_SIZE_REG, nHW);	//for Rev.A backward compatible
	reg_writel(VIDEO_SIZE_REG0 + nId, nHW);	//for Rev.B or later only

	if (((nHeight == 240) || (nHeight == 288)) && (nWidth >= 700))
		nWidth = 720;

	if (((nHeight == 240) || (nHeight == 288)) && (nWidth > 699))
		nWidth = 720;
	else
		nWidth = (16 * nWidth / 720) + nWidth;

	//decoder  Scale 
	nW = nWidth & 0x7FF;
	nW = (720 * 256) / nW;
	nH = nHeight & 0x1FF;

	///if (nHeight >240)   //PAL
	if (dev->PAL50[nId + 1]) {
		nH = (288 * 256) / nH;
	} else			//NTSC
	{
		nH = (240 * 256) / nH;
	}

	if (nId >= 4) {
		nAddr = VSCALE1_LO + ((nId - 4) << 4) + 0x100;
	} else
		nAddr = VSCALE1_LO + (nId << 4);

	nVal = nH & 0xFF;	//V

	reg_writel(nAddr, nVal);
	nReg = reg_readl(nAddr);

	nAddr++;		//V H
	nVal = (((nH >> 8) & 0xF) << 4) | ((nW >> 8) & 0xF);

	reg_writel(nAddr, nVal);
	nReg = reg_readl(nAddr);

	nAddr++;		//H
	nVal = nW & 0xFF;

	if (nId < 4) {
		reg_writel(nAddr, nVal);
		nReg = reg_readl(nAddr);
	}

	reg_writel(nAddr, nVal);
	nReg = reg_readl(nAddr);

	nAddr++;		//H
	nVal = nW & 0xFF;

	if (nId < 4) {
		reg_writel(nAddr, nVal);
		nReg = reg_readl(nAddr);
	}

	reg_writel(nAddr, nVal);
	nReg = reg_readl(nAddr);

// H Scaler
	nVal = (nWidth - 12 - 4) * (1 << 16) / nWidth;
	nVal = (4 & 0x1F) | (((nWidth - 12) & 0x3FF) << 5) | (nVal << 15);

	reg_writel(SHSCALER_REG0 + nId, nVal);
}

void resync(unsigned long data)
{
	struct TW68_dev *dev = (struct TW68_dev *)data;
	u32 dwRegE, dwRegF, k, m, mask;
	unsigned long now = jiffies;

	mod_timer(&dev->delay_resync, jiffies + msecs_to_jiffies(50));

	if (now - dev->errlog[0] < msecs_to_jiffies(50)) {
		return;
	}

	m = 0;
	mask = 0;
	for (k = 0; k < 16; k++) {
		mask = ((dev->videoDMA_ID ^ dev->videoCap_ID) & (1 << k));
		if ((mask)) {
			m++;
			dev->videoRS_ID |= mask;
			if ((m > 1) || dev->videoDMA_ID)
				break;
		}
	}

	if ((dev->videoDMA_ID == 0) && dev->videoRS_ID) {
		dev->videoDMA_ID = dev->videoRS_ID;
		dwRegE = dev->videoDMA_ID;

		/* enable DMA channels */
		reg_writel(DMA_CHANNEL_ENABLE, dwRegE);
		dwRegE = reg_readl(DMA_CHANNEL_ENABLE);
		dwRegF = (1 << 31); /* DMA enable bit */
		dwRegF |= dwRegE;
		/* reset DMA channels */
		reg_writel(DMA_CMD, dwRegF);
		dwRegF = reg_readl(DMA_CMD);

		dev->videoRS_ID = 0;
	}
}

void TW68_buffer_timeout(unsigned long data)
{
	u32 dwRegE, dwRegF;

	struct TW68_dmaqueue *q = (struct TW68_dmaqueue *)data;
	struct TW68_dev *dev = q->dev;

	if (q->curr) {

		dwRegE = reg_readl(DMA_CHANNEL_ENABLE);
		dwRegF = reg_readl(DMA_CMD);

		TW68_buffer_finish(dev, q, VIDEOBUF_ERROR);
	}
	TW68_buffer_next(dev, q);
}

int TW68_set_dmabits(struct TW68_dev *dev, unsigned int DMA_nCH)
{
	u32 dwRegST, dwRegER, dwRegPB, dwRegE, dwRegF, nId, k, run;
	nId = DMA_nCH;

	dwRegST = reg_readl(DMA_INT_STATUS);

	dwRegER = reg_readl(DMA_INT_ERROR);

	dwRegPB = reg_readl(DMA_PB_STATUS);

	dwRegE = reg_readl(DMA_CHANNEL_ENABLE);

	dwRegF = reg_readl(DMA_CMD);

	dev->video_DMA_1st_started += 1;
	dev->video_dmaq[DMA_nCH].FieldPB = 0;

	dwRegE |= (1 << nId);

	dev->videoCap_ID |= (1 << nId);
	dev->videoDMA_ID |= (1 << nId);
	reg_writel(DMA_CHANNEL_ENABLE, dwRegE);

	run = 0;

	for (k = 0; k < 8; k++) {
		if (run < dev->videoDMA_run[k])
			run = dev->videoDMA_run[k];
	}

	dev->videoDMA_run[nId] = run + 1;
	dwRegF = (1 << 31);
	dwRegF |= dwRegE;
	reg_writel(DMA_CMD, dwRegF);
	return 0;
}

int stop_video_DMA(struct TW68_dev *dev, unsigned int DMA_nCH)
{
	u32 dwRegER, dwRegPB, dwRegE, dwRegF, nId;
	nId = DMA_nCH;		///2;

	dwRegER = reg_readl(DMA_INT_ERROR);
	dwRegPB = reg_readl(DMA_PB_STATUS);
	dwRegE = reg_readl(DMA_CHANNEL_ENABLE);
	dwRegF = reg_readl(DMA_CMD);

	dwRegE &= ~(1 << nId);
	reg_writel(DMA_CHANNEL_ENABLE, dwRegE);
	dwRegE = reg_readl(DMA_CHANNEL_ENABLE);
	dev->videoDMA_ID &= ~(1 << nId);
	dev->videoCap_ID &= ~(1 << nId);

	dwRegF &= ~(1 << nId);
	reg_writel(DMA_CMD, dwRegF);
	dwRegF = reg_readl(DMA_CMD);

	dev->videoDMA_run[nId] = 0;

	if (dev->videoCap_ID == 0) {
		reg_writel(DMA_CMD, 0);
		reg_writel(DMA_CHANNEL_ENABLE, 0);
	}

	return 0;
}

int VideoDecoderDetect(struct TW68_dev *dev, unsigned int DMA_nCH)
{
	u32 regDW, dwReg;

	if (DMA_nCH < 4)	// VD 1-4
	{
		regDW = reg_readl(DECODER0_STATUS + (DMA_nCH * 0x10));
		dwReg = reg_readl(DECODER0_SDT + (DMA_nCH * 0x10));
	} else			// 6869  VD 5-8
	{
		regDW =
		    reg_readl(DECODER0_STATUS + ((DMA_nCH - 4) * 0x10) + 0x100);
		dwReg =
		    reg_readl(DECODER0_SDT + ((DMA_nCH - 4) * 0x10) + 0x100);
	}

	if ((regDW & 1))	//&& (!(dwReg & 0x80)))   ///skip the detection glitch     //detect properly
	{
		// set to PAL 50 for real...
		return 50;
	} else {
		return 60;
	}
}

void video_tasklet(unsigned long device)
{
	u32 dwRegST, dwRegPB, k;
	struct TW68_dev *dev = (struct TW68_dev *)device;

	dwRegST = dev->dwRegST;
	dwRegPB = dev->dwRegPB;

	for (k = 0; k < 8; k++) {
		if ((dwRegST & dev->videoDMA_ID) & (1 << k))	/// exclude  inactive dev
		{
			TW68_irq_video_done(dev, k + 1, dwRegPB);

			if (dev->video_dmaq[k + 1].FieldPB & 0xF0) {
				dev->video_dmaq[k + 1].FieldPB &= 0xFFFF0000;
			} else {
				dev->video_dmaq[k + 1].FieldPB &= 0xFFFF00FF;	// clear  PB
				dev->video_dmaq[k + 1].FieldPB |=
				    (dwRegPB & (1 << k)) << 8;
				dev->video_dmaq[k + 1].FCN++;
			}
		}
	}
}

static irqreturn_t TW68_irq(int irq, void *dev_id)	/// hardware dev id for the ISR
{
	struct TW68_dev *dev = (struct TW68_dev *)dev_id;
	unsigned long flags, k, eno, handled;
	u32 dwRegST, dwRegER, dwRegPB, dwRegE, dwRegF, dwRegVP, dwErrBit;
	static u32 lastPB = 0;

	handled = 1;

	spin_lock_irqsave(&dev->slock, flags);

	dwRegST = reg_readl(DMA_INT_STATUS);
	dwRegER = reg_readl(DMA_INT_ERROR);
	dwRegPB = reg_readl(DMA_PB_STATUS);
	dwRegE = reg_readl(DMA_CHANNEL_ENABLE);
	dwRegVP = reg_readl(VIDEO_PARSER_STATUS);
	dwRegF = reg_readl(DMA_CMD);
	spin_unlock_irqrestore(&dev->slock, flags);

	if ((dwRegER & DMA_FIFO_OVFERR_MASK) && dev->video_DMA_1st_started
	    && dev->err_times < 9) {
		dev->video_DMA_1st_started--;

		if (dev->video_DMA_1st_started < 0)
			dev->video_DMA_1st_started = 0;

		dev->err_times++;
		printk
		    ("DeviceInterrupt: 1st startup err_times:%d ## dma_status (err) =0x%x   dwRegVP (video parser)=0X%x   int_status 0x%x   dwRegE 0X%x \n",
		     dev->err_times, dwRegER, dwRegVP, dwRegST, dwRegE);
	} else {
		if ((dwRegER & DMA_FIFO_ANYERR_MASK)
			|| dwRegVP || (dwRegST & DMA_STAT_BADFMT_MASK))
		{
			//stop
			//err_exist, such as cpl error, tlp error, time-out
			dwErrBit = 0;
			dwErrBit |= ((dwRegST >> 24) & 0xFF);
			dwErrBit |= (((dwRegVP >> 8) | dwRegVP) & 0xFF);
			dwErrBit |=
			    (((dwRegER >> 24) | (dwRegER >> 16)) & 0xFF);

			eno = 0;
			for (k = 0; k < 8; k++) {
				if (dwErrBit & (1 << k)) {
					eno++;
					// Disable DMA channel
					dwRegE &= ~((1 << k));
					if (eno > 2)
						dwRegE &= ~((0xFF));
				}
			}

			// stop  all error channels
			spin_lock_irqsave(&dev->slock, flags);
			dev->videoDMA_ID = dwRegE;
			reg_writel(DMA_CHANNEL_ENABLE, dwRegE);
			dwRegE = reg_readl(DMA_CHANNEL_ENABLE);
			dwRegF = (1 << 31);
			dwRegF |= dwRegE;
			reg_writel(DMA_CMD, dwRegF);
			dwRegF = reg_readl(DMA_CMD);

			spin_unlock_irqrestore(&dev->slock, flags);

			printk
			    ("DeviceInterrupt: errors  ## dma_status  0x%X   (err) =0x%X   dwRegVP (video parser)=0X%x   int_status 0x%x  # dwRegE 0X%x dwRegF 0X%x \n",
			     dwErrBit, dwRegER, dwRegVP, dwRegST, dwRegE,
			     dwRegF);
			dev->errlog[0] = jiffies;

		} else {
			if (dwRegST & (0xFF00) & dev->videoDMA_ID) {
				TW68_alsa_irq(dev, dwRegST, dwRegPB);
			}

			// lastPB is always 0 ?!
			if ((lastPB != dwRegPB) && (dwRegST & (0xFF)) &&
					(!(dwRegER & DMA_FIFO_ANYERR_MASK))) {
				dev->dwRegPB = dwRegPB;
				dev->dwRegST = dwRegST;
				tasklet_schedule(&dev->vid_tasklet);
			}

			if (dev->videoRS_ID) {
				dev->videoDMA_ID |= dev->videoRS_ID;
				dev->videoRS_ID = 0;
				dwRegE = dev->videoDMA_ID;

				reg_writel(DMA_CHANNEL_ENABLE, dwRegE);
				dwRegF = reg_readl(DMA_CHANNEL_ENABLE);
				dwRegF = (1 << 31);
				dwRegF |= dwRegE;
				reg_writel(DMA_CMD, dwRegF);
				dwRegF = reg_readl(DMA_CMD);
			}
		}
	}

	if (!dwRegER && !dwRegST)	// skip the  interrupt  conflicts
	{
		if (dev->videoCap_ID == 0) {
			reg_writel(DMA_CMD, 0);
			reg_writel(DMA_CHANNEL_ENABLE, 0);
			dwRegE = reg_readl(DMA_CHANNEL_ENABLE);
			dwRegF = reg_readl(DMA_CMD);
		}

		handled = 0;
	}

	return IRQ_RETVAL(handled);
}

/* ------------------------------------------------------------------ */

/* early init (no i2c, no irq) */

static int TW68_hwinit1(struct TW68_dev *dev)
{
	u32 m_StartIdx, m_EndIdx, m_nVideoFormat, m_dwCHConfig, dwReg,
	    m_bHorizontalDecimate = 0, m_bVerticalDecimate =
	    0, m_nDropChannelNum, m_bDropMasterOrSlave, m_bDropField,
	    m_bDropOddOrEven, m_nCurVideoChannelNum;

	u32 regDW, k, ChannelOffset, pgn;

	// Audio P
	int audio_ch;
	u32 dmaP, dmaB;

	mutex_init(&dev->lock);
	spin_lock_init(&dev->slock);

	pci_read_config_dword(dev->pci, PCI_COMMAND, &regDW);	// 04 PCI_COMMAND
	regDW |= 7;
	regDW &= 0xfffffbff;
	pci_write_config_dword(dev->pci, PCI_COMMAND, regDW);


	// MSI CAP     disable MSI
	pci_read_config_dword(dev->pci, 0x50, &regDW);
	regDW &= 0xfffeffff;
	pci_write_config_dword(dev->pci, 0x50, regDW);

	//  MSIX  CAP    disable
	pci_read_config_dword(dev->pci, 0xac, &regDW);
	regDW &= 0x7fffffff;
	pci_write_config_dword(dev->pci, 0xac, regDW);

	pci_read_config_dword(dev->pci, 0x78, &regDW);
	regDW &= 0xfffffe1f;
	regDW |= (0x8 << 5);	///  8 - 128   ||  9 - 256  || A - 512
	pci_write_config_dword(dev->pci, 0x78, regDW);

	mdelay(20);
	reg_writel(DMA_CHANNEL_ENABLE, 0);
	mdelay(50);
	reg_writel(DMA_CMD, 0);

	reg_readl(DMA_CHANNEL_ENABLE);
	reg_readl(DMA_CMD);

	//Trasmit Posted FC credit Status
	reg_writel(EP_REG_ADDR, 0x730);	//

	//Trasnmit Non-Posted FC credit Status
	reg_writel(EP_REG_ADDR, 0x734);	//

	//CPL FC credit Status
	reg_writel(EP_REG_ADDR, 0x738);	//

	reg_writel((SYS_SOFT_RST), 0x01);	//??? 01   09
	reg_writel((SYS_SOFT_RST), 0x0F);

	regDW = 0x1518;
	reg_writel(PHASE_REF_CONFIG, regDW & 0xFFFF);

	//  Allocate PB DMA pagetable  total 16K  filled with 0xFF
	videoDMA_pgtable_alloc(dev->pci, &dev->m_Page0);
	AudioDMA_PB_alloc(dev->pci, &dev->m_AudioBuffer);

	for (k = 0; k < 8; k++) {
		if (dma_field_alloc
		    (&dev->Field_P[k], 720 * 600 * 2, dev->pci,
		     PCI_DMA_BIDIRECTIONAL))
		{
			dma_field_free(&dev->Field_P[k]);
			return -1;
		}
		if (dma_field_alloc
		    (&dev->Field_B[k], 720 * 600 * 2, dev->pci,
		     PCI_DMA_BIDIRECTIONAL))
		{
			dma_field_free(&dev->Field_B[k]);
			return -1;
		}
	}

	ChannelOffset = pgn = 128;	///125;
	pgn = 85;		///   starting for 720 * 240 * 2
	m_nDropChannelNum = 0;
	m_bDropMasterOrSlave = 1;	// master
	m_bDropField = 0;
	m_bDropOddOrEven = 0;

	m_nVideoFormat = VIDEO_FORMAT_YUYV;
	for (k = 0; k < MAX_NUM_SG_DMA; k++) {
		m_StartIdx = ChannelOffset * k;
		m_EndIdx = m_StartIdx + pgn;
		m_nCurVideoChannelNum = 0;	// real-time video channel  starts 0
		m_nVideoFormat = 0;	///0; ///VIDEO_FORMAT_UYVY;

		m_dwCHConfig = (m_StartIdx & 0x3FF) |	// 10 bits
		    ((m_EndIdx & 0x3FF) << 10) |	// 10 bits
		    ((m_nVideoFormat & 7) << 20) |
		    ((m_bHorizontalDecimate & 1) << 23) |
		    ((m_bVerticalDecimate & 1) << 24) |
		    ((m_nDropChannelNum & 3) << 25) |
		    ((m_bDropMasterOrSlave & 1) << 27) |	// 1 bit
		    ((m_bDropField & 1) << 28) |
		    ((m_bDropOddOrEven & 1) << 29) |
		    ((m_nCurVideoChannelNum & 3) << 30);

		reg_writel(DMA_CH0_CONFIG + k, m_dwCHConfig);
		dwReg = reg_readl(DMA_CH0_CONFIG + k);

		reg_writel(VERTICAL_CTRL, 0x24);	//0x26 will cause ch0 and ch1 have dma_error.  0x24
		reg_writel(LOOP_CTRL, 0xA5);	// 0xfd   0xA5     /// 1005
		reg_writel(DROP_FIELD_REG0 + k, 0);	///m_nDropFiledReg
	}

// setup audio DMA
	for (audio_ch = 0; audio_ch < MAX_NUM_DATA_DMA; audio_ch++) {
		dmaP = dev->m_AudioBuffer.dma + (PAGE_SIZE << 1) * audio_ch;
		dmaB =
		    dev->m_AudioBuffer.dma + (PAGE_SIZE << 1) * audio_ch +
		    PAGE_SIZE;

		reg_writel(DMA_CH8_CONFIG_P + audio_ch * 2, dmaP);
		/// Audio B = P+1
		reg_writel(DMA_CH8_CONFIG_B + audio_ch * 2, dmaB);
	}


	reg_writel((DMA_PAGE_TABLE0_ADDR), dev->m_Page0.dma);	//P DMA page table
	reg_writel((DMA_PAGE_TABLE1_ADDR), dev->m_Page0.dma + (PAGE_SIZE * 2));	//B DMA page table
	reg_writel(AVSRST, 0x3F);	// u32
	reg_writel(DMA_CMD, 0);	// u32
	reg_writel(DMA_CHANNEL_ENABLE, 0);
	reg_writel(DMA_CHANNEL_TIMEOUT, 0x3EFF0FF0);	// longer timeout setting
	reg_writel(DMA_INT_REF, 0x38000);	///   2a000 2b000 2c000  3932e     0x3032e
	reg_writel(DMA_CONFIG, 0x00FF0004);
	regDW = (0xFF << 16) | (VIDEO_GEN_PATTERNS << 8) | VIDEO_GEN;
	reg_writel(VIDEO_CTRL2, regDW);

	//VDelay
	regDW = 0x014;
	reg_writel(VDELAY0, regDW);
	reg_writel(VDELAY1, regDW);
	reg_writel(VDELAY2, regDW);
	reg_writel(VDELAY3, regDW);
	// +4 decoder   0x100
	//   6869
	reg_writel(VDELAY0 + 0x100, regDW);
	reg_writel(VDELAY1 + 0x100, regDW);
	reg_writel(VDELAY2 + 0x100, regDW);
	reg_writel(VDELAY3 + 0x100, regDW);

	//Show Blue background if no signal
	regDW = 0xe7;
	reg_writel(MISC_CONTROL2, regDW);

	// 6869
	reg_writel(MISC_CONTROL2 + 0x100, regDW);

	// device data structure initialization
	TW68_video_init1(dev);

	// decoder parameter setup
	TW68_video_init2(dev);	// set TV param

	dev->video_DMA_1st_started = 0;	// initial value for skipping startup DMA error
	dev->err_times = 0;	// DMA error counter

	for (k = 0; k < 8; k++) {
		dev->videoDMA_run[k] = 0;;
	}

	return 0;
}

/* shutdown */
static int TW68_hwfini(struct TW68_dev *dev)
{
	return 0;
}

static int vdev_init(struct TW68_dev *dev, struct video_device *template,
		     char *type)
{
	struct video_device *vfdev[9];	/// QF 0 + 8

	int k = 1;
	int err0;

	for (k = 1; k < 9; k++)	// dev0  QF muxer  dev 1 ~ 4
	{
		vfdev[k] = video_device_alloc();

		if (NULL == vfdev[k]) {
			printk(KERN_WARNING "Null  vfdev %d **** \n\n", k);
			return k;

		}

		*(vfdev[k]) = *template;

		vfdev[k]->v4l2_dev = &dev->v4l2_dev;
		vfdev[k]->release = video_device_release;
		//vfdev[k]->debug = video_debug;
		snprintf(vfdev[k]->name, sizeof(vfdev[k]->name), "%s %s (%s22)",
			 dev->name, type, TW68_boards[dev->board].name);

		dev->video_device[k] = vfdev[k];


		err0 =
		    video_register_device(dev->video_device[k],
					  VFL_TYPE_GRABBER, video_nr[dev->nr]);
		dev->vfd_DMA_num[k] = vfdev[k]->num;
	}

	return k;
}

static void TW68_unregister_video(struct TW68_dev *dev)
{
	int k;

	for (k = 1; k < 9; k++)	/// 0 + 4
	{
		if (dev->video_device[k])
			if (-1 != dev->video_device[k]->minor) {
				video_unregister_device(dev->video_device[k]);

				printk(KERN_INFO
				       "video_unregister_device(dev->video_dev %d  \n",
				       k);
			}
	}
}

static int TW68_initdev(struct pci_dev *pci_dev,
			const struct pci_device_id *pci_id)
{
	struct TW68_dev *dev;
	int err, err0;

	if (TW68_devcount == TW68_MAXBOARDS)
		return -ENOMEM;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);

	if (NULL == dev)
		return -ENOMEM;

	err = v4l2_device_register(&pci_dev->dev, &dev->v4l2_dev);

	if (err)
		goto fail0;

	/* pci init */
	dev->pci = pci_dev;
	if (pci_enable_device(pci_dev)) {
		err = -EIO;
		goto fail1;
	}

	dev->nr = TW68_devcount;
	sprintf(dev->name, "TW%x[%d]", pci_dev->device, dev->nr);

	/* pci quirks */
	if (pci_pci_problems) {
		if (pci_pci_problems & PCIPCI_TRITON)
			printk(KERN_INFO "%s: quirk: PCIPCI_TRITON\n",
			       dev->name);
		if (pci_pci_problems & PCIPCI_NATOMA)
			printk(KERN_INFO "%s: quirk: PCIPCI_NATOMA\n",
			       dev->name);
		if (pci_pci_problems & PCIPCI_VIAETBF)
			printk(KERN_INFO "%s: quirk: PCIPCI_VIAETBF\n",
			       dev->name);
		if (pci_pci_problems & PCIPCI_VSFX)
			printk(KERN_INFO "%s: quirk: PCIPCI_VSFX\n", dev->name);
#ifdef PCIPCI_ALIMAGIK
		if (pci_pci_problems & PCIPCI_ALIMAGIK) {
			printk(KERN_INFO
			       "%s: quirk: PCIPCI_ALIMAGIK -- latency fixup\n",
			       dev->name);
			latency = 0x0A;
		}
#endif
	}


	/* print pci info */
	pci_read_config_byte(pci_dev, PCI_CLASS_REVISION, &dev->pci_rev);
	pci_read_config_byte(pci_dev, PCI_LATENCY_TIMER, &dev->pci_lat);

	printk(KERN_INFO "%s: found at %s, rev: %d, irq: %d, "
	       "latency: %d, mmio: 0x%llx\n", dev->name,
	       pci_name(pci_dev), dev->pci_rev, pci_dev->irq,
	       dev->pci_lat, (unsigned long long)pci_resource_start(pci_dev,
								    0));

	pci_set_master(pci_dev);
	pci_set_drvdata(pci_dev, &(dev->v4l2_dev));

	//if (!pci_dma_supported(pci_dev, DMA_BIT_MASK(32))) {
	if (!dma_supported(pci_dev == NULL ? NULL : &pci_dev->dev, DMA_BIT_MASK(32))) {
		printk("%s: Oops: no 32bit PCI DMA ???\n", dev->name);
		err = -EIO;
		goto fail1;
	} else
		printk("%s: Hi: 32bit PCI DMA supported \n", dev->name);

	dev->board = 1;
	printk(KERN_INFO "%s: subsystem: %04x:%04x, board: %s [card=%d,%s]\n",
	       dev->name, pci_dev->subsystem_vendor,
	       pci_dev->subsystem_device, TW68_boards[dev->board].name,
	       dev->board, dev->autodetected ?
	       "autodetected" : "insmod option");

	/* get mmio */
	if (!request_mem_region(pci_resource_start(pci_dev, 0),
				pci_resource_len(pci_dev, 0), dev->name)) {
		err = -EBUSY;
		printk(KERN_ERR "%s: can't get MMIO memory @ 0x%llx\n",
		       dev->name,
		       (unsigned long long)pci_resource_start(pci_dev, 0));
		goto fail1;
	}

	// no cache
	dev->lmmio =
	    ioremap_nocache(pci_resource_start(pci_dev, 0),
			    pci_resource_len(pci_dev, 0));

	dev->bmmio = (__u8 __iomem *) dev->lmmio;

	if (NULL == dev->lmmio) {
		err = -EIO;
		printk(KERN_ERR "%s: can't ioremap() MMIO memory\n", dev->name);
		goto fail2;
	}

	TW68_hwinit1(dev);

	/* get irq */
	err = request_irq(pci_dev->irq, TW68_irq, IRQF_SHARED, dev->name, dev);

	//kb: added for video tasklet;
	tasklet_init(&dev->vid_tasklet, video_tasklet, (unsigned long)dev);

	if (err < 0) {
		printk(KERN_ERR "%s: can't get IRQ %d\n",
		       dev->name, pci_dev->irq);
		goto fail3;
	}

	v4l2_prio_init(&dev->prio);

	list_add_tail(&dev->devlist, &TW686v_devlist);

	/* register v4l devices */
	err0 = vdev_init(dev, &TW68_video_template, "video");

	if (err0 < 0) {
		printk(KERN_INFO "%s: can't register video device\n",
		       dev->name);
		goto fail4;
	}

	TW68_devcount++;
	printk(KERN_INFO "%s: registered PCI device %d [v4l2]:%d  err: |%d| \n",
	       dev->name, TW68_devcount, dev->video_device[1]->num, err0);

	err0 = TW68_alsa_create(dev);

	return 0;

fail4:
	TW68_unregister_video(dev);
	free_irq(pci_dev->irq, dev);
fail3:
	TW68_hwfini(dev);
	iounmap(dev->lmmio);
fail2:
	release_mem_region(pci_resource_start(pci_dev, 0),
			   pci_resource_len(pci_dev, 0));
fail1:
	v4l2_device_unregister(&dev->v4l2_dev);
fail0:
	kfree(dev);
	return err;
}

static void TW68_finidev(struct pci_dev *pci_dev)
{

	int m, n, k = 0;
	struct v4l2_device *v4l2_dev = pci_get_drvdata(pci_dev);
	struct TW68_dev *dev =
	    container_of(v4l2_dev, struct TW68_dev, v4l2_dev);

	printk(KERN_INFO "%s: Starting unregister video device %d\n",
	       dev->name, dev->video_device[1]->num);

	/* shutdown hardware */
	TW68_hwfini(dev);

	/* shutdown subsystems */

	/* unregister */
	mutex_lock(&TW68_devlist_lock);
	list_del(&dev->devlist);
	mutex_unlock(&TW68_devlist_lock);
	TW68_devcount--;

	/* the DMA sound modules should be unloaded before reaching
	   this, but just in case they are still present... */
	//if (dev->dmasound.priv_data != NULL) {
	// free_irq(pci_dev->irq, &dev->dmasound);
	//      dev->dmasound.priv_data = NULL;
	//}

	del_timer(&dev->delay_resync);

	/* release resources */
	/// remove IRQ
	free_irq(pci_dev->irq, dev);	/////////  0420
	iounmap(dev->lmmio);
	release_mem_region(pci_resource_start(pci_dev, 0),
			   pci_resource_len(pci_dev, 0));

	TW68_pgtable_free(dev->pci, &dev->m_Page0);

	for (n = 0; n < 8; n++)
		for (m = 0; m < 4; m++) {
			pci_free_consistent(dev->pci, 800 * 300 * 2,
					    dev->BDbuf[n][m].cpu,
					    dev->BDbuf[n][m].dma_addr);
		}

	TW68_pgtable_free(dev->pci, &dev->m_AudioBuffer);

	for (k = 0; k < 8; k++) {
		dma_field_free(&dev->Field_P[k]);
		dma_field_free(&dev->Field_B[k]);
	}

	TW68_unregister_video(dev);
	TW68_alsa_free(dev);

	//v4l2_device_unregister(&dev->v4l2_dev);
	printk(KERN_INFO " unregistered v4l2_dev device  %d %d %d\n",
	       TW68_VERSION_CODE >> 16, (TW68_VERSION_CODE >> 8) & 0xFF,
	       TW68_VERSION_CODE & 0xFF);
	/* free memory */
	kfree(dev);
}

/* ----------------------------------------------------------- */

static struct pci_driver TW68_pci_driver = {
	.name = "TW6869",
	.id_table = TW68_pci_tbl,
	.probe = TW68_initdev,
	.remove = TW68_finidev,
};

/* ----------------------------------------------------------- */

static int TW68_init(void)
{
	INIT_LIST_HEAD(&TW686v_devlist);
	printk(KERN_INFO "TW68_: v4l2 driver version %d.%d.%d loaded\n",
	       TW68_VERSION_CODE >> 16, (TW68_VERSION_CODE >> 8) & 0xFF,
	       TW68_VERSION_CODE & 0xFF);

	return pci_register_driver(&TW68_pci_driver);
}

static void TW68_fini(void)
{
	pci_unregister_driver(&TW68_pci_driver);
	printk(KERN_INFO "TW68_: v4l2 driver version %d.%d.%d removed\n",
	       TW68_VERSION_CODE >> 16, (TW68_VERSION_CODE >> 8) & 0xFF,
	       TW68_VERSION_CODE & 0xFF);

}

module_init(TW68_init);
module_exit(TW68_fini);

