// Isolated NVMe block service. Queue memory and the bounce pool are owned by
// this process and mapped into its IOMMU domain; client channel pages are
// deliberately never DMA mapped.

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <gdos/block.h>
#include <gdos/kring_iommu.h>
#include <gdos/kring_shares.h>
#include <gdos/pci.h>

#include <kring.h>
#include <string.h>
#include <sys.h>

#define PAGE_SIZE 4096ull
#define DMA_PAGES 16
#define QUEUE_ENTRIES 64
#define MAX_CLIENTS 8

#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_ADMIN_IDENTIFY  0x06
#define NVME_IO_WRITE        0x01
#define NVME_IO_READ         0x02

struct [[gnu::packed]] nvme_command {
  uint32_t cdw0;
  uint32_t nsid;
  uint64_t reserved;
  uint64_t mptr;
  uint64_t prp1;
  uint64_t prp2;
  uint32_t cdw10;
  uint32_t cdw11;
  uint32_t cdw12;
  uint32_t cdw13;
  uint32_t cdw14;
  uint32_t cdw15;
};

struct [[gnu::packed]] nvme_completion {
  uint32_t result;
  uint32_t reserved;
  uint16_t sq_head;
  uint16_t sq_id;
  uint16_t cid;
  uint16_t status;
};

struct nvme_queue {
  struct nvme_command *sq;
  volatile struct nvme_completion *cq;
  uint16_t qid;
  uint16_t sq_tail;
  uint16_t cq_head;
  uint16_t next_cid;
  bool phase;
};

struct nvme_device {
  uint64_t bar;
  uint64_t dma;
  uint32_t doorbell_stride;
  struct nvme_queue admin;
  struct nvme_queue io;
  uint64_t capacity_blocks;
  uint32_t block_size;
  uint32_t max_transfer_blocks;
};

struct block_client {
  struct gdos_block_channel *channel;
  uint64_t bytes;
  uint32_t seen;
};

static uint64_t command(struct kring *ring, uint64_t op, uint64_t a,
                        uint64_t b, uint64_t c) {
  struct ksqe *sqe = kring_get_sqe(ring);
  if (sqe == nullptr)
    return SYSERR_AGAIN;
  *sqe = (struct ksqe){.op = op, .a = a, .b = b, .c = c};
  uint64_t rc = kring_submit(ring);
  if (rc != 0)
    return rc;
  for (;;) {
    struct kcqe completion;
    rc = kring_wait_cqe(ring, &completion);
    if (rc != 0)
      return rc;
    kring_ack(ring);
    if (completion.type == op)
      return completion.status;
  }
}

static void signal_state(struct pci_driver_start *setup, uint32_t state) {
  atomic_store_explicit(&setup->state, state, memory_order_release);
  sys_block_doorbell((uint64_t)setup);
}

static bool wait_state(struct pci_driver_start *setup, uint32_t wanted) {
  for (;;) {
    uint32_t state =
        atomic_load_explicit(&setup->state, memory_order_acquire);
    if (state >= wanted)
      return true;
    if (sys_block_wait(&setup->state, state) != 0)
      return false;
  }
}

static bool wait_ready(volatile uint32_t *csts, bool ready) {
  for (uint32_t i = 0; i < 10000000; i++) {
    if (((*csts & 1) != 0) == ready)
      return true;
    __asm__ volatile("pause");
  }
  return false;
}

static volatile uint32_t *doorbell(const struct nvme_device *dev,
                                   uint16_t qid, bool completion) {
  uint64_t index = 2ull * qid + (completion ? 1 : 0);
  return (volatile uint32_t *)(dev->bar + 0x1000 +
                               index * dev->doorbell_stride);
}

static bool nvme_submit(struct nvme_device *dev, struct nvme_queue *q,
                        const struct nvme_command *input,
                        struct nvme_completion *result) {
  uint16_t cid = ++q->next_cid;
  if (cid == 0)
    cid = ++q->next_cid;
  struct nvme_command cmd = *input;
  cmd.cdw0 = (cmd.cdw0 & 0xffffu) | ((uint32_t)cid << 16);
  q->sq[q->sq_tail] = cmd;
  atomic_thread_fence(memory_order_release);
  q->sq_tail = (uint16_t)((q->sq_tail + 1) % QUEUE_ENTRIES);
  *doorbell(dev, q->qid, false) = q->sq_tail;

  for (uint32_t spin = 0; spin < 10000000; spin++) {
    struct nvme_completion c = q->cq[q->cq_head];
    if ((bool)(c.status & 1) != q->phase) {
      __asm__ volatile("pause");
      continue;
    }
    if (c.cid != cid)
      return false;
    if (result != nullptr)
      *result = c;
    q->cq_head++;
    if (q->cq_head == QUEUE_ENTRIES) {
      q->cq_head = 0;
      q->phase = !q->phase;
    }
    atomic_thread_fence(memory_order_release);
    *doorbell(dev, q->qid, true) = q->cq_head;
    return (c.status & 0xfffeu) == 0;
  }
  return false;
}

static bool identify(struct nvme_device *dev, uint32_t nsid, uint32_t cns,
                     uint64_t buffer) {
  memset((void *)buffer, 0, PAGE_SIZE);
  struct nvme_command cmd = {
      .cdw0 = NVME_ADMIN_IDENTIFY,
      .nsid = nsid,
      .prp1 = buffer,
      .cdw10 = cns,
  };
  return nvme_submit(dev, &dev->admin, &cmd, nullptr);
}

static bool setup_io_queues(struct nvme_device *dev) {
  struct nvme_command cq = {
      .cdw0 = NVME_ADMIN_CREATE_CQ,
      .prp1 = dev->dma + 3 * PAGE_SIZE,
      .cdw10 = 1u | ((QUEUE_ENTRIES - 1u) << 16),
      .cdw11 = 1u | (1u << 1), // physically contiguous, IRQ enabled, vector 0
  };
  if (!nvme_submit(dev, &dev->admin, &cq, nullptr))
    return false;
  struct nvme_command sq = {
      .cdw0 = NVME_ADMIN_CREATE_SQ,
      .prp1 = dev->dma + 2 * PAGE_SIZE,
      .cdw10 = 1u | ((QUEUE_ENTRIES - 1u) << 16),
      .cdw11 = 1u | (1u << 16), // physically contiguous, completion queue 1
  };
  return nvme_submit(dev, &dev->admin, &sq, nullptr);
}

static bool discover_namespace(struct nvme_device *dev) {
  uint8_t *controller = (void *)(dev->dma + 4 * PAGE_SIZE);
  uint8_t *ns = (void *)(dev->dma + 5 * PAGE_SIZE);
  if (!identify(dev, 0, 1, (uint64_t)controller) ||
      !identify(dev, 1, 0, (uint64_t)ns))
    return false;
  uint32_t nn;
  uint64_t nsze;
  memcpy(&nn, controller + 516, sizeof(nn));
  memcpy(&nsze, ns, sizeof(nsze));
  uint8_t format = ns[26] & 0xf;
  if (nn == 0 || format >= 16 || nsze == 0)
    return false;
  uint8_t lbads = ns[128 + (uint32_t)format * 4 + 2];
  uint16_t metadata_size;
  memcpy(&metadata_size, ns + 128 + (uint32_t)format * 4,
         sizeof(metadata_size));
  if (metadata_size != 0 || lbads < 9 || lbads > 12)
    return false;
  dev->capacity_blocks = nsze;
  dev->block_size = 1u << lbads;
  dev->max_transfer_blocks = (8 * PAGE_SIZE) / dev->block_size;
  uint8_t mdts = controller[77];
  if (mdts != 0 && mdts < 32) {
    uint32_t mpsmin = (*(volatile uint64_t *)dev->bar >> 48) & 0xf;
    if (12 + mpsmin + mdts >= 63)
      return false;
    uint64_t controller_max = 1ull << (12 + mpsmin + mdts);
    uint64_t blocks = controller_max / dev->block_size;
    if (blocks < dev->max_transfer_blocks)
      dev->max_transfer_blocks = (uint32_t)blocks;
  }
  return dev->max_transfer_blocks != 0;
}

static bool nvme_rw(struct nvme_device *dev, bool write, uint64_t lba,
                    uint32_t blocks) {
  uint64_t bytes = (uint64_t)blocks * dev->block_size;
  uint64_t bounce = dev->dma + 7 * PAGE_SIZE;
  uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  uint64_t prp2 = 0;
  if (pages == 2) {
    prp2 = bounce + PAGE_SIZE;
  } else if (pages > 2) {
    uint64_t *list = (void *)(dev->dma + 6 * PAGE_SIZE);
    memset(list, 0, PAGE_SIZE);
    for (uint64_t i = 1; i < pages; i++)
      list[i - 1] = bounce + i * PAGE_SIZE;
    prp2 = (uint64_t)list;
  }
  struct nvme_command cmd = {
      .cdw0 = write ? NVME_IO_WRITE : NVME_IO_READ,
      .nsid = 1,
      .prp1 = bounce,
      .prp2 = prp2,
      .cdw10 = (uint32_t)lba,
      .cdw11 = (uint32_t)(lba >> 32),
      .cdw12 = blocks - 1,
  };
  return nvme_submit(dev, &dev->io, &cmd, nullptr);
}

static bool channel_range(uint64_t bytes, uint32_t offset, uint32_t length) {
  return offset >= sizeof(struct gdos_block_channel) &&
         (uint64_t)offset + length >= offset &&
         (uint64_t)offset + length <= bytes;
}

static void serve_request(struct nvme_device *dev, struct block_client *client,
                          uint32_t seq) {
  struct gdos_block_channel *ch = client->channel;
  uint32_t status = GDOS_BLOCK_STATUS_OK;
  uint32_t op = ch->op;
  uint64_t lba = ch->lba;
  uint32_t blocks = ch->block_count;
  uint32_t offset = ch->data_offset;
  uint32_t length = ch->data_length;
  uint64_t wanted = (uint64_t)blocks * dev->block_size;

  if (ch->magic != GDOS_BLOCK_MAGIC || ch->version != GDOS_BLOCK_VERSION ||
      ch->header_bytes != sizeof(*ch)) {
    status = GDOS_BLOCK_STATUS_BAD_VERSION;
  } else if (op != GDOS_BLOCK_INFO && op != GDOS_BLOCK_READ &&
             op != GDOS_BLOCK_WRITE) {
    status = GDOS_BLOCK_STATUS_BAD_REQUEST;
  } else if (op != GDOS_BLOCK_INFO &&
             (blocks == 0 || blocks > dev->max_transfer_blocks ||
              lba >= dev->capacity_blocks ||
              blocks > dev->capacity_blocks - lba || length != wanted ||
              !channel_range(client->bytes, offset, length))) {
    status = GDOS_BLOCK_STATUS_RANGE;
  } else if (op != GDOS_BLOCK_INFO) {
    void *arena = (uint8_t *)ch + offset;
    void *bounce = (void *)(dev->dma + 7 * PAGE_SIZE);
    if (op == GDOS_BLOCK_WRITE)
      memcpy(bounce, arena, length);
    if (!nvme_rw(dev, op == GDOS_BLOCK_WRITE, lba, blocks)) {
      status = GDOS_BLOCK_STATUS_IO;
    } else if (op == GDOS_BLOCK_READ) {
      memcpy(arena, bounce, length);
    }
  }

  ch->capacity_blocks = dev->capacity_blocks;
  ch->logical_block_size = dev->block_size;
  ch->max_transfer_blocks = dev->max_transfer_blocks;
  ch->status = status;
  client->seen = seq;
  atomic_store_explicit(&ch->response_seq, seq, memory_order_release);
  sys_block_doorbell((uint64_t)ch);
}

static void add_client(struct block_client clients[MAX_CLIENTS], uint64_t base,
                       uint8_t order) {
  struct gdos_block_channel *ch = (void *)base;
  if (ch->magic != GDOS_BLOCK_MAGIC || ch->version != GDOS_BLOCK_VERSION ||
      ch->header_bytes != sizeof(*ch))
    return;
  for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].channel == ch)
      return;
    if (clients[i].channel == nullptr) {
      clients[i] = (struct block_client){
          .channel = ch,
          .bytes = PAGE_SIZE << order,
          .seen = atomic_load_explicit(&ch->response_seq,
                                       memory_order_acquire),
      };
      return;
    }
  }
  sys_vm_unshare(base);
}

static void drain_shares(struct kring *shares,
                         struct block_client clients[MAX_CLIENTS]) {
  bool consumed = false;
  const struct kcqe *event;
  while ((event = kring_peek_cqe(shares)) != nullptr) {
    struct kcqe copy = *event;
    kring_cqe_seen(shares);
    consumed = true;
    if (copy.type == KEV_SHARE)
      add_client(clients, copy.b & ~0xfffull, copy.b & 0xfff);
  }
  if (consumed)
    kring_ack(shares);
}

static bool wait_irq_event(struct kring *irq) {
  for (uint32_t spin = 0; spin < 100000; spin++) {
    const struct kcqe *event = kring_peek_cqe(irq);
    if (event == nullptr) {
      sys_yield();
      continue;
    }
    bool irq_event = event->type == KEV_IRQ;
    kring_cqe_seen(irq);
    kring_ack(irq);
    if (irq_event)
      return true;
  }
  return false;
}

static void service_loop(struct nvme_device *dev,
                         struct pci_driver_start *setup, struct kring *shares,
                         struct kring *irq, struct kring *iommu) {
  struct block_client clients[MAX_CLIENTS] = {0};
  drain_shares(shares, clients);
  if (setup->service_channel != 0)
    add_client(clients, setup->service_channel, 0);
  for (;;) {
    if (atomic_load_explicit(&setup->state, memory_order_acquire) ==
        PCI_DRIVER_STOP)
      return;
    drain_shares(shares, clients);
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
      if (clients[i].channel == nullptr)
        continue;
      uint32_t seq = atomic_load_explicit(&clients[i].channel->request_seq,
                                          memory_order_acquire);
      if (seq != clients[i].seen)
        serve_request(dev, &clients[i], seq);
    }
    const struct kcqe *event;
    bool consumed = false;
    while ((event = kring_peek_cqe(irq)) != nullptr) {
      kring_cqe_seen(irq);
      consumed = true;
    }
    if (consumed)
      kring_ack(irq);
    consumed = false;
    while ((event = kring_peek_cqe(iommu)) != nullptr) {
      kring_cqe_seen(iommu);
      consumed = true;
    }
    if (consumed)
      kring_ack(iommu);
    sys_yield();
  }
}

void _start(uint64_t arg) {
  struct pci_driver_start *setup = (void *)arg;
  if (setup == nullptr || setup->version != PCI_DRIVER_START_VERSION ||
      setup->n_bars == 0) {
    print("nvmed: invalid start record\n");
    sys_exit();
  }
  struct nvme_device dev = {.bar = setup->bars[0].base};
  uint64_t cap = *(volatile uint64_t *)dev.bar;
  if ((cap & 0xffffu) + 1 < QUEUE_ENTRIES || ((cap >> 37) & 1) == 0 ||
      ((cap >> 48) & 0xf) != 0) {
    print("nvmed: unsupported controller capabilities\n");
    sys_exit();
  }
  dev.doorbell_stride = 4u << ((cap >> 32) & 0xf);
  dev.admin = (struct nvme_queue){.qid = 0, .phase = true};
  dev.io = (struct nvme_queue){.qid = 1, .phase = true};
  volatile uint32_t *cc = (volatile uint32_t *)(dev.bar + 0x14);
  volatile uint32_t *csts = (volatile uint32_t *)(dev.bar + 0x1c);
  *cc = 0;
  if (!wait_ready(csts, false)) {
    print("nvmed: reset timeout\n");
    sys_exit();
  }

  struct kring iommu;
  if (kring_create(&iommu, KSCHEME_IOMMU, PAGE_SIZE) != 0)
    sys_exit();
  dev.dma = sys_vm_alloc(DMA_PAGES * PAGE_SIZE,
                         VM_PROT_READ | VM_PROT_WRITE);
  if (sys_iserr(dev.dma))
    sys_exit();
  memset((void *)dev.dma, 0, DMA_PAGES * PAGE_SIZE);
  dev.admin.sq = (void *)dev.dma;
  dev.admin.cq = (void *)(dev.dma + PAGE_SIZE);
  dev.io.sq = (void *)(dev.dma + 2 * PAGE_SIZE);
  dev.io.cq = (void *)(dev.dma + 3 * PAGE_SIZE);
  const uint64_t domain = 1;
  if (command(&iommu, KIOMMU_DOMAIN_CREATE, domain, 0, 0) != 0 ||
      command(&iommu, KIOMMU_MAP_BLOCK, domain, dev.dma,
              IOMMU_PERM_DEVICE_READ | IOMMU_PERM_DEVICE_WRITE) != 0 ||
      command(&iommu, KIOMMU_DEVICE_ATTACH, domain, setup->requester_id,
              setup->function_id) != 0) {
    print("nvmed: IOMMU setup failed\n");
    sys_exit();
  }
  signal_state(setup, PCI_DRIVER_IOMMU_READY);

  if (!wait_state(setup, PCI_DRIVER_IRQ_GRANTED))
    sys_exit();
  struct kring irq;
  if (kring_create(&irq, KSCHEME_IRQ, PAGE_SIZE) != 0 ||
      kring_irq_bind(&irq, setup->irq_route, setup->function_id) != 0)
    sys_exit();
  struct kcqe bind;
  if (kring_wait_cqe(&irq, &bind) != 0 || bind.type != KIRQ_BIND ||
      bind.status != 0)
    sys_exit();
  kring_ack(&irq);
  signal_state(setup, PCI_DRIVER_IRQ_READY);
  if (!wait_state(setup, PCI_DRIVER_LIVE))
    sys_exit();

  *(volatile uint32_t *)(dev.bar + 0x24) =
      (QUEUE_ENTRIES - 1u) | ((QUEUE_ENTRIES - 1u) << 16);
  *(volatile uint64_t *)(dev.bar + 0x28) = dev.dma;
  *(volatile uint64_t *)(dev.bar + 0x30) = dev.dma + PAGE_SIZE;
  atomic_thread_fence(memory_order_release);
  *cc = 1u | (6u << 16) | (4u << 20);
  if (!wait_ready(csts, true)) {
    print("nvmed: enable timeout\n");
    sys_exit();
  }
  print("nvmed: live with isolated queues and bounce pool\n");

  if (!discover_namespace(&dev)) {
    print("nvmed: namespace discovery FAILED\n");
    sys_exit();
  }
  print("nvmed: namespace blocks=");
  print_hex(dev.capacity_blocks);
  print(wait_irq_event(&irq) ? "nvmed: MSI-X event delivered to driver\n"
                             : "nvmed: MSI-X event MISSING\n");

  // Preserve the kernel-memory-PRP acceptance check on the first instance.
  if (setup->function_id == 1) {
    struct nvme_command bad = {
        .cdw0 = NVME_ADMIN_IDENTIFY,
        .prp1 = 0x1000,
        .cdw10 = 1,
    };
    uint16_t tail = dev.admin.sq_tail;
    uint16_t cid = ++dev.admin.next_cid;
    bad.cdw0 |= (uint32_t)cid << 16;
    dev.admin.sq[tail] = bad;
    atomic_thread_fence(memory_order_release);
    dev.admin.sq_tail = (tail + 1) % QUEUE_ENTRIES;
    *doorbell(&dev, 0, false) = dev.admin.sq_tail;
    bool saw_fault = false;
    for (uint32_t i = 0; i < 1000000 && !saw_fault; i++) {
      const struct kcqe *event = kring_peek_cqe(&iommu);
      if (event != nullptr) {
        saw_fault = event->type == KEV_IOMMU_FAULT;
        kring_cqe_seen(&iommu);
        kring_ack(&iommu);
      } else {
        sys_yield();
      }
    }
    print(saw_fault ? "nvmed: bad PRP contained by IOMMU\n"
                    : "nvmed: IOMMU fault event MISSING\n");
    print("nvmed: exiting first instance for reap/restart test\n");
    sys_exit();
  }

  if (!setup_io_queues(&dev)) {
    print("nvmed: I/O queue creation FAILED\n");
    sys_exit();
  }
  struct kring shares;
  if (kring_create(&shares, KSCHEME_SHARES, PAGE_SIZE) != 0)
    sys_exit();
  print("nvmed: block service ready\n");
  service_loop(&dev, setup, &shares, &irq, &iommu);

  *cc = 0;
  wait_ready(csts, false);
  signal_state(setup, PCI_DRIVER_DMA_STOPPED);
  command(&iommu, KIOMMU_DEVICE_DETACH, domain, setup->requester_id, 0);
  command(&iommu, KIOMMU_UNMAP_BLOCK, domain, dev.dma, 0);
  command(&iommu, KIOMMU_DOMAIN_DESTROY, domain, 0, 0);
  kring_destroy(&shares);
  kring_destroy(&iommu);
  kring_destroy(&irq);
  sys_vm_free(dev.dma);
  sys_exit();
}
