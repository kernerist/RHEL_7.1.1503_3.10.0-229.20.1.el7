/*
 * Copyright IBM Corp. 2012
 *
 * Author(s):
 *   Jan Glauber <jang@linux.vnet.ibm.com>
 *
 * The System z PCI code is a rewrite from a prototype by
 * the following people (Kudoz!):
 *   Alexander Schmidt
 *   Christoph Raisch
 *   Hannes Hering
 *   Hoang-Nam Nguyen
 *   Jan-Bernd Themann
 *   Stefan Roscher
 *   Thomas Klein
 */

#define COMPONENT "zPCI"
#define pr_fmt(fmt) COMPONENT ": " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/kernel_stat.h>
#include <linux/seq_file.h>
#include <linux/pci.h>
#include <linux/msi.h>

#include <asm/isc.h>
#include <asm/airq.h>
#include <asm/facility.h>
#include <asm/pci_insn.h>
#include <asm/pci_clp.h>
#include <asm/pci_dma.h>

#define DEBUG				/* enable pr_debug */

#define	SIC_IRQ_MODE_ALL		0
#define	SIC_IRQ_MODE_SINGLE		1

#define ZPCI_NR_DMA_SPACES		1
#define ZPCI_NR_DEVICES			CONFIG_PCI_NR_FUNCTIONS

/* list of all detected zpci devices */
static LIST_HEAD(zpci_list);
static DEFINE_SPINLOCK(zpci_list_lock);

static DECLARE_BITMAP(zpci_domain, ZPCI_NR_DEVICES);
static DEFINE_SPINLOCK(zpci_domain_lock);

struct callback {
	irq_handler_t	handler;
	void		*data;
};

struct zdev_irq_map {
	struct airq_iv *aibv;		/* Adapter interrupt bit vector */
	struct callback	*cb;		/* callback handler array */
	int msi_vecs;			/* consecutive MSI-vectors used */
};

static struct airq_iv *zpci_aisb_iv;
static struct zdev_irq_map *zpci_imap[ZPCI_NR_DEVICES];

/* Adapter interrupt definitions */
static void zpci_irq_handler(struct airq_struct *airq);

static struct airq_struct zpci_airq = {
	.handler = zpci_irq_handler,
	.isc = PCI_ISC,
};

/* I/O Map */
static DEFINE_SPINLOCK(zpci_iomap_lock);
static DECLARE_BITMAP(zpci_iomap, ZPCI_IOMAP_MAX_ENTRIES);
struct zpci_iomap_entry *zpci_iomap_start;
EXPORT_SYMBOL_GPL(zpci_iomap_start);

static struct kmem_cache *zdev_irq_cache;
static struct kmem_cache *zdev_fmb_cache;

static inline int irq_to_msi_nr(unsigned int irq)
{
	return irq & ZPCI_MSI_VEC_MASK;
}

static inline int irq_to_dev_nr(unsigned int irq)
{
	return irq >> ZPCI_MSI_VEC_BITS;
}

struct zpci_dev *get_zdev(struct pci_dev *pdev)
{
	return (struct zpci_dev *) pdev->sysdata;
}

struct zpci_dev *get_zdev_by_fid(u32 fid)
{
	struct zpci_dev *tmp, *zdev = NULL;

	spin_lock(&zpci_list_lock);
	list_for_each_entry(tmp, &zpci_list, entry) {
		if (tmp->fid == fid) {
			zdev = tmp;
			break;
		}
	}
	spin_unlock(&zpci_list_lock);
	return zdev;
}

static struct zpci_dev *get_zdev_by_bus(struct pci_bus *bus)
{
	return (bus && bus->sysdata) ? (struct zpci_dev *) bus->sysdata : NULL;
}

int pci_domain_nr(struct pci_bus *bus)
{
	return ((struct zpci_dev *) bus->sysdata)->domain;
}
EXPORT_SYMBOL_GPL(pci_domain_nr);

int pci_proc_domain(struct pci_bus *bus)
{
	return pci_domain_nr(bus);
}
EXPORT_SYMBOL_GPL(pci_proc_domain);

/* Modify PCI: Register adapter interruptions */
static int zpci_set_airq(struct zpci_dev *zdev)
{
	u64 req = ZPCI_CREATE_REQ(zdev->fh, 0, ZPCI_MOD_FC_REG_INT);
	struct zpci_fib fib = {0};

	fib.isc = PCI_ISC;
	fib.sum = 1;		/* enable summary notifications */
	fib.noi = airq_iv_end(zdev->aibv);
	fib.aibv = (unsigned long) zdev->aibv->vector;
	fib.aibvo = 0;		/* each zdev has its own interrupt vector */
	fib.aisb = (unsigned long) zpci_aisb_iv->vector + (zdev->aisb/64)*8;
	fib.aisbo = zdev->aisb & 63;

	return zpci_mod_fc(req, &fib);
}

struct mod_pci_args {
	u64 base;
	u64 limit;
	u64 iota;
	u64 fmb_addr;
};

static int mod_pci(struct zpci_dev *zdev, int fn, u8 dmaas, struct mod_pci_args *args)
{
	u64 req = ZPCI_CREATE_REQ(zdev->fh, dmaas, fn);
	struct zpci_fib fib = {0};

	fib.pba = args->base;
	fib.pal = args->limit;
	fib.iota = args->iota;
	fib.fmb_addr = args->fmb_addr;

	return zpci_mod_fc(req, &fib);
}

/* Modify PCI: Register I/O address translation parameters */
int zpci_register_ioat(struct zpci_dev *zdev, u8 dmaas,
		       u64 base, u64 limit, u64 iota)
{
	struct mod_pci_args args = { base, limit, iota, 0 };

	WARN_ON_ONCE(iota & 0x3fff);
	args.iota |= ZPCI_IOTA_RTTO_FLAG;
	return mod_pci(zdev, ZPCI_MOD_FC_REG_IOAT, dmaas, &args);
}

/* Modify PCI: Unregister I/O address translation parameters */
int zpci_unregister_ioat(struct zpci_dev *zdev, u8 dmaas)
{
	struct mod_pci_args args = { 0, 0, 0, 0 };

	return mod_pci(zdev, ZPCI_MOD_FC_DEREG_IOAT, dmaas, &args);
}

/* Modify PCI: Unregister adapter interruptions */
static int zpci_clear_airq(struct zpci_dev *zdev)
{
	struct mod_pci_args args = { 0, 0, 0, 0 };

	return mod_pci(zdev, ZPCI_MOD_FC_DEREG_INT, 0, &args);
}

/* Modify PCI: Set PCI function measurement parameters */
int zpci_fmb_enable_device(struct zpci_dev *zdev)
{
	struct mod_pci_args args = { 0, 0, 0, 0 };

	if (zdev->fmb)
		return -EINVAL;

	zdev->fmb = kmem_cache_zalloc(zdev_fmb_cache, GFP_KERNEL);
	if (!zdev->fmb)
		return -ENOMEM;
	WARN_ON((u64) zdev->fmb & 0xf);

	args.fmb_addr = virt_to_phys(zdev->fmb);
	return mod_pci(zdev, ZPCI_MOD_FC_SET_MEASURE, 0, &args);
}

/* Modify PCI: Disable PCI function measurement */
int zpci_fmb_disable_device(struct zpci_dev *zdev)
{
	struct mod_pci_args args = { 0, 0, 0, 0 };
	int rc;

	if (!zdev->fmb)
		return -EINVAL;

	/* Function measurement is disabled if fmb address is zero */
	rc = mod_pci(zdev, ZPCI_MOD_FC_SET_MEASURE, 0, &args);

	kmem_cache_free(zdev_fmb_cache, zdev->fmb);
	zdev->fmb = NULL;
	return rc;
}

#define ZPCI_PCIAS_CFGSPC	15

static int zpci_cfg_load(struct zpci_dev *zdev, int offset, u32 *val, u8 len)
{
	u64 req = ZPCI_CREATE_REQ(zdev->fh, ZPCI_PCIAS_CFGSPC, len);
	u64 data;
	int rc;

	rc = zpci_load(&data, req, offset);
	if (!rc) {
		data = data << ((8 - len) * 8);
		data = le64_to_cpu(data);
		*val = (u32) data;
	} else
		*val = 0xffffffff;
	return rc;
}

static int zpci_cfg_store(struct zpci_dev *zdev, int offset, u32 val, u8 len)
{
	u64 req = ZPCI_CREATE_REQ(zdev->fh, ZPCI_PCIAS_CFGSPC, len);
	u64 data = val;
	int rc;

	data = cpu_to_le64(data);
	data = data >> ((8 - len) * 8);
	rc = zpci_store(data, req, offset);
	return rc;
}

void enable_irq(unsigned int irq)
{
	struct msi_desc *msi = irq_get_msi_desc(irq);

	zpci_msi_set_mask_bits(msi, 1, 0);
}
EXPORT_SYMBOL_GPL(enable_irq);

void disable_irq(unsigned int irq)
{
	struct msi_desc *msi = irq_get_msi_desc(irq);

	zpci_msi_set_mask_bits(msi, 1, 1);
}
EXPORT_SYMBOL_GPL(disable_irq);

void pcibios_fixup_bus(struct pci_bus *bus)
{
}

resource_size_t pcibios_align_resource(void *data, const struct resource *res,
				       resource_size_t size,
				       resource_size_t align)
{
	return 0;
}

/* combine single writes by using store-block insn */
void __iowrite64_copy(void __iomem *to, const void *from, size_t count)
{
       zpci_memcpy_toio(to, from, count);
}

/* Create a virtual mapping cookie for a PCI BAR */
void __iomem *pci_iomap(struct pci_dev *pdev, int bar, unsigned long max)
{
	struct zpci_dev *zdev =	get_zdev(pdev);
	u64 addr;
	int idx;

	if ((bar & 7) != bar)
		return NULL;

	idx = zdev->bars[bar].map_idx;
	spin_lock(&zpci_iomap_lock);
	zpci_iomap_start[idx].fh = zdev->fh;
	zpci_iomap_start[idx].bar = bar;
	spin_unlock(&zpci_iomap_lock);

	addr = ZPCI_IOMAP_ADDR_BASE | ((u64) idx << 48);
	return (void __iomem *) addr;
}
EXPORT_SYMBOL_GPL(pci_iomap);

void pci_iounmap(struct pci_dev *pdev, void __iomem *addr)
{
	unsigned int idx;

	idx = (((__force u64) addr) & ~ZPCI_IOMAP_ADDR_BASE) >> 48;
	spin_lock(&zpci_iomap_lock);
	zpci_iomap_start[idx].fh = 0;
	zpci_iomap_start[idx].bar = 0;
	spin_unlock(&zpci_iomap_lock);
}
EXPORT_SYMBOL_GPL(pci_iounmap);

static int pci_read(struct pci_bus *bus, unsigned int devfn, int where,
		    int size, u32 *val)
{
	struct zpci_dev *zdev = get_zdev_by_bus(bus);
	int ret;

	if (!zdev || devfn != ZPCI_DEVFN)
		ret = -ENODEV;
	else
		ret = zpci_cfg_load(zdev, where, val, size);

	return ret;
}

static int pci_write(struct pci_bus *bus, unsigned int devfn, int where,
		     int size, u32 val)
{
	struct zpci_dev *zdev = get_zdev_by_bus(bus);
	int ret;

	if (!zdev || devfn != ZPCI_DEVFN)
		ret = -ENODEV;
	else
		ret = zpci_cfg_store(zdev, where, val, size);

	return ret;
}

static struct pci_ops pci_root_ops = {
	.read = pci_read,
	.write = pci_write,
};

static void zpci_irq_handler(struct airq_struct *airq)
{
	unsigned long si, ai;
	struct zdev_irq_map *imap;
	int irqs_on = 0;

	inc_irq_stat(IRQIO_PCI);
	for (si = 0;;) {
		/* Scan adapter summary indicator bit vector */
		si = airq_iv_scan(zpci_aisb_iv, si, airq_iv_end(zpci_aisb_iv));
		if (si == -1UL) {
			if (irqs_on++)
				/* End of second scan with interrupts on. */
				break;
			/* First scan complete, reenable interrupts. */
			zpci_set_irq_ctrl(SIC_IRQ_MODE_SINGLE, NULL, PCI_ISC);
			si = 0;
			continue;
		}

		/* Scan the adapter interrupt vector for this device. */
		imap = zpci_imap[si];
		for (ai = 0;;) {
			ai = airq_iv_scan(imap->aibv, ai, imap->msi_vecs);
			if (ai == -1UL)
				break;
			inc_irq_stat(IRQIO_MSI);
			airq_iv_lock(imap->aibv, ai);
			if (imap->cb[ai].handler)
				imap->cb[ai].handler(ai, imap->cb[ai].data);
			airq_iv_unlock(imap->aibv, ai);
		}
	}
}

static int zpci_alloc_msi(struct zpci_dev *zdev, int msi_vecs)
{
	unsigned long size;

	/* Alloc aibv & callback space */
	zdev->irq_map = kmem_cache_zalloc(zdev_irq_cache, GFP_KERNEL);
	if (!zdev->irq_map)
		goto out;
	/* Store the number of used MSI vectors */
	zdev->irq_map->msi_vecs = msi_vecs;
	/* Allocate callback array */
	size = sizeof(struct callback) * msi_vecs;
	zdev->irq_map->cb = kzalloc(size, GFP_KERNEL);
	if (!zdev->irq_map->cb)
		goto out_map;
	/* Allocate msi_map array */
	size = sizeof(struct msi_map) * msi_vecs;
	zdev->msi_map = kzalloc(size, GFP_KERNEL);
	if (!zdev->msi_map)
		goto out_cb;
	return 0;

out_cb:
	kfree(zdev->irq_map->cb);
out_map:
	kmem_cache_free(zdev_irq_cache, zdev->irq_map);
out:
	return -ENOMEM;
}

static void zpci_free_msi(struct zpci_dev *zdev)
{
	kfree(zdev->msi_map);
	kfree(zdev->irq_map->cb);
	kmem_cache_free(zdev_irq_cache, zdev->irq_map);
}

int arch_setup_msi_irqs(struct pci_dev *pdev, int nvec, int type)
{
	struct zpci_dev *zdev = get_zdev(pdev);
	unsigned int msi_nr, msi_vecs;
	unsigned long aisb;
	struct msi_desc *msi;
	int rc;

	if (type == PCI_CAP_ID_MSI && nvec > 1)
		return 1;
	msi_vecs = min(nvec, ZPCI_MSI_VEC_MAX);

	/* Allocate adapter summary indicator bit */
	rc = -EIO;
	aisb = airq_iv_alloc_bit(zpci_aisb_iv);
	if (aisb == -1UL)
		goto out;
	zdev->aisb = aisb;

	/* Create adapter interrupt vector */
	rc = -ENOMEM;
	zdev->aibv = airq_iv_create(msi_vecs, AIRQ_IV_BITLOCK);
	if (!zdev->aibv)
		goto out_si;

	/* Allocate data structures for msi interrupts */
	rc = zpci_alloc_msi(zdev, msi_vecs);
	if (rc)
		goto out_iv;

	/* Wire up shortcut pointer */
	zpci_imap[aisb] = zdev->irq_map;
	zdev->irq_map->aibv = zdev->aibv;

	/*
	 * TODO: irq number 0 wont be found if we return less than the
	 * requested MSIs. Ignore it for now and fix in common code.
	 */
	msi_nr = aisb << ZPCI_MSI_VEC_BITS;
	list_for_each_entry(msi, &pdev->msi_list, list) {
		rc = zpci_setup_msi_irq(zdev, msi, msi_nr,
					  aisb << ZPCI_MSI_VEC_BITS);
		if (rc)
			goto out_msi;
		msi_nr++;
	}

	/* Enable adapter interrupts */
	rc = zpci_set_airq(zdev);
	if (rc)
		goto out_msi;

	return (msi_vecs == nvec) ? 0 : msi_vecs;

out_msi:
	msi_nr -= aisb << ZPCI_MSI_VEC_BITS;
	list_for_each_entry(msi, &pdev->msi_list, list) {
		if (msi_nr-- == 0)
			break;
		zpci_teardown_msi_irq(zdev, msi);
	}
	zpci_free_msi(zdev);
out_iv:
	airq_iv_release(zdev->aibv);
out_si:
	airq_iv_free_bit(zpci_aisb_iv, aisb);
out:
	return rc;
}

void arch_teardown_msi_irqs(struct pci_dev *pdev)
{
	struct zpci_dev *zdev = get_zdev(pdev);
	struct msi_desc *msi;
	int rc;

	/* Disable adapter interrupts */
	rc = zpci_clear_airq(zdev);
	if (rc)
		return;

	list_for_each_entry(msi, &pdev->msi_list, list)
		zpci_teardown_msi_irq(zdev, msi);

	zpci_free_msi(zdev);
	airq_iv_release(zdev->aibv);
	airq_iv_free_bit(zpci_aisb_iv, zdev->aisb);
}

static void zpci_map_resources(struct zpci_dev *zdev)
{
	struct pci_dev *pdev = zdev->pdev;
	resource_size_t len;
	int i;

	for (i = 0; i < PCI_BAR_COUNT; i++) {
		len = pci_resource_len(pdev, i);
		if (!len)
			continue;
		pdev->resource[i].start = (resource_size_t) pci_iomap(pdev, i, 0);
		pdev->resource[i].end = pdev->resource[i].start + len - 1;
	}
}

static void zpci_unmap_resources(struct zpci_dev *zdev)
{
	struct pci_dev *pdev = zdev->pdev;
	resource_size_t len;
	int i;

	for (i = 0; i < PCI_BAR_COUNT; i++) {
		len = pci_resource_len(pdev, i);
		if (!len)
			continue;
		pci_iounmap(pdev, (void *) pdev->resource[i].start);
	}
}

int pcibios_add_platform_entries(struct pci_dev *pdev)
{
	return zpci_sysfs_add_device(&pdev->dev);
}

int zpci_request_irq(unsigned int irq, irq_handler_t handler, void *data)
{
	unsigned int msi_nr = irq_to_msi_nr(irq);
	unsigned int dev_nr = irq_to_dev_nr(irq);
	struct zdev_irq_map *imap;
	struct msi_desc *msi;

	msi = irq_get_msi_desc(irq);
	if (!msi)
		return -EIO;

	imap = zpci_imap[dev_nr];
	imap->cb[msi_nr].handler = handler;
	imap->cb[msi_nr].data = data;

	/*
	 * The generic MSI code returns with the interrupt disabled on the
	 * card, using the MSI mask bits. Firmware doesn't appear to unmask
	 * at that level, so we do it here by hand.
	 */
	zpci_msi_set_mask_bits(msi, 1, 0);
	return 0;
}

void zpci_free_irq(unsigned int irq)
{
	unsigned int msi_nr = irq_to_msi_nr(irq);
	unsigned int dev_nr = irq_to_dev_nr(irq);
	struct zdev_irq_map *imap;
	struct msi_desc *msi;

	/* Disable interrupt */
	msi = irq_get_msi_desc(irq);
	if (!msi)
		return;
	zpci_msi_set_mask_bits(msi, 1, 1);
	imap = zpci_imap[dev_nr];
	imap->cb[msi_nr].handler = NULL;
	imap->cb[msi_nr].data = NULL;
	synchronize_rcu();
}

int request_irq(unsigned int irq, irq_handler_t handler,
		unsigned long irqflags, const char *devname, void *dev_id)
{
	return zpci_request_irq(irq, handler, dev_id);
}
EXPORT_SYMBOL_GPL(request_irq);

void free_irq(unsigned int irq, void *dev_id)
{
	zpci_free_irq(irq);
}
EXPORT_SYMBOL_GPL(free_irq);

static int __init zpci_irq_init(void)
{
	int rc;

	rc = register_adapter_interrupt(&zpci_airq);
	if (rc)
		goto out;
	/* Set summary to 1 to be called every time for the ISC. */
	*zpci_airq.lsi_ptr = 1;

	rc = -ENOMEM;
	zpci_aisb_iv = airq_iv_create(ZPCI_NR_DEVICES, AIRQ_IV_ALLOC);
	if (!zpci_aisb_iv)
		goto out_airq;

	zpci_set_irq_ctrl(SIC_IRQ_MODE_SINGLE, NULL, PCI_ISC);
	return 0;

out_airq:
	unregister_adapter_interrupt(&zpci_airq);
out:
	return rc;
}

static void zpci_irq_exit(void)
{
	airq_iv_release(zpci_aisb_iv);
	unregister_adapter_interrupt(&zpci_airq);
}

static int zpci_alloc_iomap(struct zpci_dev *zdev)
{
	int entry;

	spin_lock(&zpci_iomap_lock);
	entry = find_first_zero_bit(zpci_iomap, ZPCI_IOMAP_MAX_ENTRIES);
	if (entry == ZPCI_IOMAP_MAX_ENTRIES) {
		spin_unlock(&zpci_iomap_lock);
		return -ENOSPC;
	}
	set_bit(entry, zpci_iomap);
	spin_unlock(&zpci_iomap_lock);
	return entry;
}

static void zpci_free_iomap(struct zpci_dev *zdev, int entry)
{
	spin_lock(&zpci_iomap_lock);
	memset(&zpci_iomap_start[entry], 0, sizeof(struct zpci_iomap_entry));
	clear_bit(entry, zpci_iomap);
	spin_unlock(&zpci_iomap_lock);
}

static struct resource *__alloc_res(struct zpci_dev *zdev, unsigned long start,
				    unsigned long size, unsigned long flags)
{
	struct resource *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return NULL;

	r->start = start;
	r->end = r->start + size - 1;
	r->flags = flags;
	r->name = zdev->res_name;

	if (request_resource(&iomem_resource, r)) {
		kfree(r);
		return NULL;
	}
	return r;
}

static int zpci_setup_bus_resources(struct zpci_dev *zdev,
				    struct list_head *resources)
{
	unsigned long addr, size, flags;
	struct resource *res;
	int i, entry;

	snprintf(zdev->res_name, sizeof(zdev->res_name),
		 "PCI Bus %04x:%02x", zdev->domain, ZPCI_BUS_NR);

	for (i = 0; i < PCI_BAR_COUNT; i++) {
		if (!zdev->bars[i].size)
			continue;
		entry = zpci_alloc_iomap(zdev);
		if (entry < 0)
			return entry;
		zdev->bars[i].map_idx = entry;

		/* only MMIO is supported */
		flags = IORESOURCE_MEM;
		if (zdev->bars[i].val & 8)
			flags |= IORESOURCE_PREFETCH;
		if (zdev->bars[i].val & 4)
			flags |= IORESOURCE_MEM_64;

		addr = ZPCI_IOMAP_ADDR_BASE + ((u64) entry << 48);

		size = 1UL << zdev->bars[i].size;

		res = __alloc_res(zdev, addr, size, flags);
		if (!res) {
			zpci_free_iomap(zdev, entry);
			return -ENOMEM;
		}
		zdev->bars[i].res = res;
		pci_add_resource(resources, res);
	}

	return 0;
}

static void zpci_cleanup_bus_resources(struct zpci_dev *zdev)
{
	int i;

	for (i = 0; i < PCI_BAR_COUNT; i++) {
		if (!zdev->bars[i].size)
			continue;

		zpci_free_iomap(zdev, zdev->bars[i].map_idx);
		release_resource(zdev->bars[i].res);
		kfree(zdev->bars[i].res);
	}
}

int pcibios_add_device(struct pci_dev *pdev)
{
	struct zpci_dev *zdev = get_zdev(pdev);
	struct resource *res;
	int i;

	zdev->pdev = pdev;
	zpci_map_resources(zdev);

	for (i = 0; i < PCI_BAR_COUNT; i++) {
		res = &pdev->resource[i];
		if (res->parent || !res->flags)
			continue;
		pci_claim_resource(pdev, i);
	}

	return 0;
}

int pcibios_enable_device(struct pci_dev *pdev, int mask)
{
	struct zpci_dev *zdev = get_zdev(pdev);

	zdev->pdev = pdev;
	zpci_debug_init_device(zdev);
	zpci_fmb_enable_device(zdev);
	zpci_map_resources(zdev);

	return pci_enable_resources(pdev, mask);
}

void pcibios_disable_device(struct pci_dev *pdev)
{
	struct zpci_dev *zdev = get_zdev(pdev);

	zpci_unmap_resources(zdev);
	zpci_fmb_disable_device(zdev);
	zpci_debug_exit_device(zdev);
	zdev->pdev = NULL;
}

#ifdef CONFIG_HIBERNATE_CALLBACKS
static int zpci_restore(struct device *dev)
{
	struct zpci_dev *zdev = get_zdev(to_pci_dev(dev));
	int ret = 0;

	if (zdev->state != ZPCI_FN_STATE_ONLINE)
		goto out;

	ret = clp_enable_fh(zdev, ZPCI_NR_DMA_SPACES);
	if (ret)
		goto out;

	zpci_map_resources(zdev);
	zpci_register_ioat(zdev, 0, zdev->start_dma + PAGE_OFFSET,
			   zdev->start_dma + zdev->iommu_size - 1,
			   (u64) zdev->dma_table);

out:
	return ret;
}

static int zpci_freeze(struct device *dev)
{
	struct zpci_dev *zdev = get_zdev(to_pci_dev(dev));

	if (zdev->state != ZPCI_FN_STATE_ONLINE)
		return 0;

	zpci_unregister_ioat(zdev, 0);
	return clp_disable_fh(zdev);
}

struct dev_pm_ops pcibios_pm_ops = {
	.thaw_noirq = zpci_restore,
	.freeze_noirq = zpci_freeze,
	.restore_noirq = zpci_restore,
	.poweroff_noirq = zpci_freeze,
};
#endif /* CONFIG_HIBERNATE_CALLBACKS */

static int zpci_alloc_domain(struct zpci_dev *zdev)
{
	spin_lock(&zpci_domain_lock);
	zdev->domain = find_first_zero_bit(zpci_domain, ZPCI_NR_DEVICES);
	if (zdev->domain == ZPCI_NR_DEVICES) {
		spin_unlock(&zpci_domain_lock);
		return -ENOSPC;
	}
	set_bit(zdev->domain, zpci_domain);
	spin_unlock(&zpci_domain_lock);
	return 0;
}

static void zpci_free_domain(struct zpci_dev *zdev)
{
	spin_lock(&zpci_domain_lock);
	clear_bit(zdev->domain, zpci_domain);
	spin_unlock(&zpci_domain_lock);
}

void pcibios_remove_bus(struct pci_bus *bus)
{
	struct zpci_dev *zdev = get_zdev_by_bus(bus);

	zpci_exit_slot(zdev);
	zpci_cleanup_bus_resources(zdev);
	zpci_free_domain(zdev);

	spin_lock(&zpci_list_lock);
	list_del(&zdev->entry);
	spin_unlock(&zpci_list_lock);

	kfree(zdev);
}

static int zpci_scan_bus(struct zpci_dev *zdev)
{
	LIST_HEAD(resources);
	int ret;

	ret = zpci_setup_bus_resources(zdev, &resources);
	if (ret)
		return ret;

	zdev->bus = pci_scan_root_bus(NULL, ZPCI_BUS_NR, &pci_root_ops,
				      zdev, &resources);
	if (!zdev->bus) {
		zpci_cleanup_bus_resources(zdev);
		return -EIO;
	}

	zdev->bus->max_bus_speed = zdev->max_bus_speed;
	return 0;
}

int zpci_enable_device(struct zpci_dev *zdev)
{
	int rc;

	rc = clp_enable_fh(zdev, ZPCI_NR_DMA_SPACES);
	if (rc)
		goto out;

	rc = zpci_dma_init_device(zdev);
	if (rc)
		goto out_dma;

	zdev->state = ZPCI_FN_STATE_ONLINE;
	return 0;

out_dma:
	clp_disable_fh(zdev);
out:
	return rc;
}
EXPORT_SYMBOL_GPL(zpci_enable_device);

int zpci_disable_device(struct zpci_dev *zdev)
{
	zpci_dma_exit_device(zdev);
	return clp_disable_fh(zdev);
}
EXPORT_SYMBOL_GPL(zpci_disable_device);

int zpci_create_device(struct zpci_dev *zdev)
{
	int rc;

	rc = zpci_alloc_domain(zdev);
	if (rc)
		goto out;

	if (zdev->state == ZPCI_FN_STATE_CONFIGURED) {
		rc = zpci_enable_device(zdev);
		if (rc)
			goto out_free;
	}
	rc = zpci_scan_bus(zdev);
	if (rc)
		goto out_disable;

	spin_lock(&zpci_list_lock);
	list_add_tail(&zdev->entry, &zpci_list);
	spin_unlock(&zpci_list_lock);

	zpci_init_slot(zdev);

	return 0;

out_disable:
	if (zdev->state == ZPCI_FN_STATE_ONLINE)
		zpci_disable_device(zdev);
out_free:
	zpci_free_domain(zdev);
out:
	return rc;
}

void zpci_stop_device(struct zpci_dev *zdev)
{
	zpci_dma_exit_device(zdev);
	/*
	 * Note: SCLP disables fh via set-pci-fn so don't
	 * do that here.
	 */
}
EXPORT_SYMBOL_GPL(zpci_stop_device);

static inline int barsize(u8 size)
{
	return (size) ? (1 << size) >> 10 : 0;
}

static int zpci_mem_init(void)
{
	zdev_irq_cache = kmem_cache_create("PCI_IRQ_cache", sizeof(struct zdev_irq_map),
				L1_CACHE_BYTES, SLAB_HWCACHE_ALIGN, NULL);
	if (!zdev_irq_cache)
		goto error_zdev;

	zdev_fmb_cache = kmem_cache_create("PCI_FMB_cache", sizeof(struct zpci_fmb),
				16, 0, NULL);
	if (!zdev_fmb_cache)
		goto error_fmb;

	/* TODO: use realloc */
	zpci_iomap_start = kzalloc(ZPCI_IOMAP_MAX_ENTRIES * sizeof(*zpci_iomap_start),
				   GFP_KERNEL);
	if (!zpci_iomap_start)
		goto error_iomap;
	return 0;

error_iomap:
	kmem_cache_destroy(zdev_fmb_cache);
error_fmb:
	kmem_cache_destroy(zdev_irq_cache);
error_zdev:
	return -ENOMEM;
}

static void zpci_mem_exit(void)
{
	kfree(zpci_iomap_start);
	kmem_cache_destroy(zdev_irq_cache);
	kmem_cache_destroy(zdev_fmb_cache);
}

static unsigned int s390_pci_probe;
static unsigned int s390_pci_initialized;

char * __init pcibios_setup(char *str)
{
	if (!strcmp(str, "on")) {
		s390_pci_probe = 1;
		return NULL;
	}
	return str;
}

bool zpci_is_enabled(void)
{
	return s390_pci_initialized;
}

static int __init pci_base_init(void)
{
	int rc;

	if (!s390_pci_probe)
		return 0;

	if (!test_facility(2) || !test_facility(69)
	    || !test_facility(71) || !test_facility(72))
		return 0;

	rc = zpci_debug_init();
	if (rc)
		return rc;

	rc = zpci_mem_init();
	if (rc)
		goto out_mem;

	rc = zpci_msihash_init();
	if (rc)
		goto out_hash;

	rc = zpci_irq_init();
	if (rc)
		goto out_irq;

	rc = zpci_dma_init();
	if (rc)
		goto out_dma;

	rc = clp_scan_pci_devices();
	if (rc)
		goto out_find;

	s390_pci_initialized = 1;
	return 0;

out_find:
	zpci_dma_exit();
out_dma:
	zpci_irq_exit();
out_irq:
	zpci_msihash_exit();
out_hash:
	zpci_mem_exit();
out_mem:
	zpci_debug_exit();
	return rc;
}
subsys_initcall_sync(pci_base_init);

void zpci_rescan(void)
{
	if (zpci_is_enabled())
		clp_rescan_pci_devices_simple();
}
