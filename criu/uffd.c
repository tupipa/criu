#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include "linux/userfaultfd.h"

#include "asm/page.h"
#include "log.h"
#include "criu-plugin.h"
#include "page-read.h"
#include "files-reg.h"
#include "kerndat.h"
#include "mem.h"
#include "uffd.h"
#include "util-pie.h"
#include "protobuf.h"
#include "pstree.h"
#include "crtools.h"
#include "cr_options.h"
#include "xmalloc.h"
#include "syscall-codes.h"
#include "restorer.h"

#undef  LOG_PREFIX
#define LOG_PREFIX "lazy-pages: "

struct lazy_pages_info {
	int pid;
	int uffd;

	struct list_head pages;

	unsigned long total_pages;
	unsigned long copied_pages;

	struct hlist_node hash;
};

#define LPI_HASH_SIZE	16
static struct hlist_head lpi_hash[LPI_HASH_SIZE];

static struct lazy_pages_info *lpi_init(void)
{
	struct lazy_pages_info *lpi = NULL;

	lpi = malloc(sizeof(*lpi));
	if (!lpi) {
		pr_err("allocation failed\n");
		return NULL;
	}

	memset(lpi, 0, sizeof(*lpi));
	INIT_LIST_HEAD(&lpi->pages);
	INIT_HLIST_NODE(&lpi->hash);

	return lpi;
}

static void lpi_fini(struct lazy_pages_info *lpi)
{
	if (lpi->uffd)
		close(lpi->uffd);
	free(lpi);
}

static void lpi_hash_init(void)
{
	int i;

	for (i = 0; i < LPI_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&lpi_hash[i]);
}

struct lazy_pages_info *uffd_to_lpi(int uffd)
{
	struct lazy_pages_info *lpi;
	struct hlist_head *head;

	head = &lpi_hash[uffd % LPI_HASH_SIZE];
	hlist_for_each_entry(lpi, head, hash)
		if (lpi->uffd == uffd)
			return lpi;

	return NULL;
}

static void lpi_hash_fini(void)
{
	struct lazy_pages_info *p;
	struct hlist_node *n;
	int i;

	for (i = 0; i < LPI_HASH_SIZE; i++)
		hlist_for_each_entry_safe(p, n, &lpi_hash[i], hash)
			lpi_fini(p);
}

static int send_uffd(int sendfd, int pid)
{
	int fd;
	int len;
	int ret = -1;
	struct sockaddr_un sun;

	if (!opts.addr) {
		pr_info("Please specify a file name for the unix domain socket\n");
		pr_info("used to communicate between the lazy-pages server\n");
		pr_info("and the restore process. Use the --address option like\n");
		pr_info("criu restore --lazy-pages --address /tmp/userfault.socket\n");
		return -1;
	}

	if (sendfd < 0)
		return -1;

	if (strlen(opts.addr) >= sizeof(sun.sun_path)) {
		return -1;
	}

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strcpy(sun.sun_path, opts.addr);
	len = offsetof(struct sockaddr_un, sun_path) + strlen(opts.addr);
	if (connect(fd, (struct sockaddr *) &sun, len) < 0) {
		pr_perror("connect to %s failed", opts.addr);
		goto out;
	}

	/* The "transfer protocol" is first the pid as int and then
	 * the FD for UFFD */
	pr_debug("Sending PID %d\n", pid);
	if (send(fd, &pid, sizeof(pid), 0) < 0) {
		pr_perror("PID sending error:");
		goto out;
	}

	if (send_fd(fd, NULL, 0, sendfd) < 0) {
		pr_perror("send_fd error:");
		goto out;
	}
	ret = 0;
out:
	close(fd);
	return ret;
}

/* Runtime detection if userfaultfd can be used */

static int check_for_uffd()
{
	int uffd;

	uffd = syscall(SYS_userfaultfd, 0);
	/*
	 * uffd == -1 is probably enough to not use lazy-restore
	 * on this system. Additionally checking for ENOSYS
	 * makes sure it is actually not implemented.
	 */
	if ((uffd == -1) && (errno == ENOSYS)) {
		pr_err("Runtime detection of userfaultfd failed on this system.\n");
		pr_err("Processes cannot be lazy-restored on this system.\n");
		return -1;
	}
	close(uffd);
	return 0;
}

/* This function is used by 'criu restore --lazy-pages' */
int setup_uffd(int pid, struct task_restore_args *task_args)
{
	struct uffdio_api uffdio_api;

	if (!opts.lazy_pages) {
		task_args->uffd = -1;
		return 0;
	}

	if (check_for_uffd())
		return -1;
	/*
	 * Open userfaulfd FD which is passed to the restorer blob and
	 * to a second process handling the userfaultfd page faults.
	 */
	task_args->uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (task_args->uffd < 0) {
		pr_perror("Unable to open an userfaultfd descriptor");
		return -1;
	}

	/*
	 * Check if the UFFD_API is the one which is expected
	 */
	uffdio_api.api = UFFD_API;
	uffdio_api.features = 0;
	if (ioctl(task_args->uffd, UFFDIO_API, &uffdio_api)) {
		pr_err("Checking for UFFDIO_API failed.\n");
		goto err;
	}
	if (uffdio_api.api != UFFD_API) {
		pr_err("Result of looking up UFFDIO_API does not match: %Lu\n", uffdio_api.api);
		goto err;
	}

	if (send_uffd(task_args->uffd, pid) < 0)
		goto err;

	return 0;
err:
	close(task_args->uffd);
	return -1;
}

static int server_listen(struct sockaddr_un *saddr)
{
	int fd;
	int len;

	if (strlen(opts.addr) >= sizeof(saddr->sun_path)) {
		return -1;
	}

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;

	unlink(opts.addr);

	memset(saddr, 0, sizeof(struct sockaddr_un));
	saddr->sun_family = AF_UNIX;
	strcpy(saddr->sun_path, opts.addr);
	len = offsetof(struct sockaddr_un, sun_path) + strlen(opts.addr);

	if (bind(fd, (struct sockaddr *) saddr, len) < 0) {
		goto out;
	}

	if (listen(fd, 10) < 0) {
		goto out;
	}

	return fd;

out:
	close(fd);
	return -1;
}

static int find_vmas(struct lazy_pages_info *lpi);

static struct lazy_pages_info *ud_open(int listen, struct sockaddr_un *saddr)
{
	struct lazy_pages_info *lpi;
	int client;
	int ret = -1;
	socklen_t len;
	int uffd_flags;

	/* accept new client request */
	len = sizeof(struct sockaddr_un);
	if ((client = accept(listen, saddr, &len)) < 0) {
		pr_perror("server_accept error: %d", client);
		close(listen);
		return NULL;
	}

	pr_debug("client fd %d\n", client);

	lpi = lpi_init();
	if (!lpi)
		goto out;

	/* The "transfer protocol" is first the pid as int and then
	 * the FD for UFFD */
	ret = recv(client, &lpi->pid, sizeof(lpi->pid), 0);
	if (ret != sizeof(lpi->pid)) {
		pr_perror("PID recv error:");
		goto out;
	}
	pr_debug("received PID: %d\n", lpi->pid);

	lpi->uffd = recv_fd(client);
	if (lpi->uffd < 0) {
		pr_perror("recv_fd error:");
		goto out;
	}
	pr_debug("lpi->uffd %d\n", lpi->uffd);
	close_safe(&client);

	pr_debug("uffd is 0x%d\n", lpi->uffd);
	uffd_flags = fcntl(lpi->uffd, F_GETFD, NULL);
	pr_debug("uffd_flags are 0x%x\n", uffd_flags);

	/*
	 * Find the memory pages belonging to the restored process
	 * so that it is trackable when all pages have been transferred.
	 */
	if ((lpi->total_pages = find_vmas(lpi)) == -1)
		goto out;

	hlist_add_head(&lpi->hash, &lpi_hash[lpi->uffd % LPI_HASH_SIZE]);

	return lpi;

out:
	free(lpi);
	close_safe(&client);
	return NULL;
}

static int get_page(struct lazy_pages_info *lpi, unsigned long addr, void *dest)
{
	struct iovec iov;
	int ret;
	unsigned char buf[PAGE_SIZE];
	struct page_read pr;

	ret = open_page_read(lpi->pid, &pr, PR_TASK | PR_MOD);
	pr_debug("get_page ret %d\n", ret);

	ret = pr.get_pagemap(&pr, &iov);
	pr_debug("get_pagemap ret %d\n", ret);
	if (ret <= 0)
		return ret;

	ret = seek_pagemap_page(&pr, addr, true);
	pr_debug("seek_pagemap_page ret 0x%x\n", ret);
	if (ret <= 0)
		return ret;

	ret = pr.read_pages(&pr, addr, 1, buf);
	pr_debug("read_pages ret %d\n", ret);
	if (ret <= 0)
		return ret;

	memcpy(dest, buf, PAGE_SIZE);

	if (pr.close)
		pr.close(&pr);

	return 1;
}

#define UFFD_FLAG_SENT	0x1

struct uffd_pages_struct {
	struct list_head list;
	unsigned long addr;
	int flags;
};

static int uffd_copy_page(struct lazy_pages_info *lpi, __u64 address,
			  void *dest)
{
	struct uffdio_copy uffdio_copy;
	int rc;

	rc = get_page(lpi, address, dest);
	if (rc <= 0)
		return rc;

	uffdio_copy.dst = address;
	uffdio_copy.src = (unsigned long) dest;
	uffdio_copy.len = page_size();
	uffdio_copy.mode = 0;
	uffdio_copy.copy = 0;

	pr_debug("uffdio_copy.dst 0x%llx\n", uffdio_copy.dst);
	rc = ioctl(lpi->uffd, UFFDIO_COPY, &uffdio_copy);
	pr_debug("ioctl UFFDIO_COPY rc 0x%x\n", rc);
	pr_debug("uffdio_copy.copy 0x%llx\n", uffdio_copy.copy);
	if (rc) {
		/* real retval in ufdio_copy.copy */
		if (uffdio_copy.copy != -EEXIST) {
			pr_err("UFFDIO_COPY error %Ld\n", uffdio_copy.copy);
			return -1;
		}
	} else if (uffdio_copy.copy != page_size()) {
		pr_err("UFFDIO_COPY unexpected size %Ld\n", uffdio_copy.copy);
		return -1;
	}

	lpi->copied_pages++;

	return uffdio_copy.copy;

}

static int uffd_zero_page(struct lazy_pages_info *lpi, __u64 address)
{
	struct uffdio_zeropage uffdio_zeropage;
	unsigned long ps = page_size();
	int rc;

	uffdio_zeropage.range.start = address;
	uffdio_zeropage.range.len = ps;
	uffdio_zeropage.mode = 0;

	pr_debug("uffdio_zeropage.range.start 0x%llx\n", uffdio_zeropage.range.start);
	rc = ioctl(lpi->uffd, UFFDIO_ZEROPAGE, &uffdio_zeropage);
	pr_debug("ioctl UFFDIO_ZEROPAGE rc 0x%x\n", rc);
	pr_debug("uffdio_zeropage.zeropage 0x%llx\n", uffdio_zeropage.zeropage);
	if (rc) {
		pr_err("UFFDIO_ZEROPAGE error %d\n", rc);
		return -1;
	}

	return ps;
}

static int uffd_handle_page(struct lazy_pages_info *lpi, __u64 address,
			    void *dest)
{
	int rc;

	rc = uffd_copy_page(lpi, address, dest);
	if (rc == 0)
		rc = uffd_zero_page(lpi, address);

	return rc;
}

static int collect_uffd_pages(struct page_read *pr, struct lazy_pages_info *lpi)
{
	unsigned long base;
	int i;
	struct iovec iov;
	unsigned long nr_pages;
	unsigned long ps;
	int rc;
	struct uffd_pages_struct *uffd_pages;
	struct vma_area *vma;
	struct vm_area_list *vmas;
	struct pstree_item *item = pstree_item_by_virt(lpi->pid);

	BUG_ON(!item);

	vmas = &rsti(item)->vmas;

	rc = pr->get_pagemap(pr, &iov);
	if (rc <= 0)
		return 0;

	ps = page_size();
	nr_pages = iov.iov_len / ps;
	base = (unsigned long) iov.iov_base;
	pr_debug("iov.iov_base 0x%lx (%ld pages)\n", base, nr_pages);

	if (pr->put_pagemap)
		pr->put_pagemap(pr);

	for (i = 0; i < nr_pages; i++) {
		bool uffd_page = false;
		base = (unsigned long) iov.iov_base + (i * ps);
		/*
		 * Only pages which are MAP_ANONYMOUS and MAP_PRIVATE
		 * are relevant for userfaultfd handling.
		 * Loop over all VMAs to see if the flags matching.
		 */
		list_for_each_entry(vma, &vmas->h, list) {
			/*
			 * This loop assumes that base can actually be found
			 * in the VMA list.
			 */
			if (base >= vma->e->start && base < vma->e->end) {
				if (vma_entry_can_be_lazy(vma->e)) {
					uffd_page = true;
					break;
				}
			}
		}

		/* This is not a page we are looking for. Move along */
		if (!uffd_page)
			continue;

		pr_debug("Adding 0x%lx to our list\n", base);

		uffd_pages = xzalloc(sizeof(struct uffd_pages_struct));
		if (!uffd_pages)
			return -1;
		uffd_pages->addr = base;
		list_add(&uffd_pages->list, &lpi->pages);
	}

	return 1;
}

static int handle_remaining_pages(struct lazy_pages_info *lpi, void *dest)
{
	struct uffd_pages_struct *uffd_pages;
	int rc;

	list_for_each_entry(uffd_pages, &lpi->pages, list) {
		pr_debug("Checking remaining pages 0x%lx (flags 0x%x)\n",
			 uffd_pages->addr, uffd_pages->flags);
		if (uffd_pages->flags & UFFD_FLAG_SENT)
			continue;

		rc = uffd_handle_page(lpi, uffd_pages->addr, dest);
		if (rc < 0) {
			pr_err("Error during UFFD copy\n");
			return -1;
		}

		uffd_pages->flags |= UFFD_FLAG_SENT;
	}

	return 0;
}


static int handle_regular_pages(struct lazy_pages_info *lpi, void *dest,
				__u64 address)
{
	int rc;
	struct uffd_pages_struct *uffd_pages;

	rc = uffd_handle_page(lpi, address, dest);
	if (rc < 0) {
		pr_err("Error during UFFD copy\n");
		return -1;
	}

	/*
	 * Mark this page as having been already transferred, so
	 * that it has not to be copied again later.
	 */
	list_for_each_entry(uffd_pages, &lpi->pages, list) {
		if (uffd_pages->addr == address)
			uffd_pages->flags |= UFFD_FLAG_SENT;
	}

	return 0;
}

/*
 *  Setting up criu infrastructure and scan for VMAs.
 */
static int find_vmas(struct lazy_pages_info *lpi)
{
	struct cr_img *img;
	int ret;
	struct vm_area_list vmas;
	int vn = 0;
	struct rst_info *ri;
	struct page_read pr;
	struct uffd_pages_struct *uffd_pages;
	struct pstree_item *item = pstree_item_by_virt(lpi->pid);

	BUG_ON(!item);

	vm_area_list_init(&vmas);

	ri = rsti(item);
	if (!ri)
		return -1;

	img = open_image(CR_FD_MM, O_RSTR, lpi->pid);
	if (!img)
		return -1;

	ret = pb_read_one_eof(img, &ri->mm, PB_MM);
	close_image(img);
	if (ret == -1)
		return -1;

	pr_debug("Found %zd VMAs in image\n", ri->mm->n_vmas);

	while (vn < ri->mm->n_vmas) {
		struct vma_area *vma;

		ret = -1;
		vma = alloc_vma_area();
		if (!vma)
			goto out;

		ret = 0;
		ri->vmas.nr++;
		vma->e = ri->mm->vmas[vn++];

		list_add_tail(&vma->list, &ri->vmas.h);

		if (vma_area_is_private(vma, kdat.task_size)) {
			vmas.priv_size += vma_area_len(vma);
			if (vma->e->flags & MAP_GROWSDOWN)
				vmas.priv_size += PAGE_SIZE;
		}

		pr_info("vma 0x%"PRIx64" 0x%"PRIx64"\n", vma->e->start, vma->e->end);
	}

	ret = open_page_read(lpi->pid, &pr, PR_TASK);
	if (ret <= 0) {
		ret = -1;
		goto out;
	}
	/*
	 * This puts all pages which should be handled by userfaultfd
	 * in the list uffd_list. This list is later used to detect if
	 * a page has already been transferred or if it needs to be
	 * pushed into the process using userfaultfd.
	 */
	do {
		ret = collect_uffd_pages(&pr, lpi);
		if (ret == -1) {
			goto out;
		}
	} while (ret);

	if (pr.close)
		pr.close(&pr);

	/* Count detected pages */
	list_for_each_entry(uffd_pages, &lpi->pages, list)
	    ret++;

	pr_debug("Found %d pages to be handled by UFFD\n", ret);

out:
	return ret;
}

static int handle_user_fault(struct lazy_pages_info *lpi, void *dest)
{
	struct uffd_msg msg;
	__u64 flags;
	__u64 address;
	struct uffd_pages_struct *uffd_pages;
	int ret;

	ret = read(lpi->uffd, &msg, sizeof(msg));
	pr_debug("read() ret: 0x%x\n", ret);
	if (!ret)
		return 1;

	if (ret != sizeof(msg)) {
		pr_perror("Can't read userfaultfd message");
		return -1;
	}

	/* Align requested address to the next page boundary */
	address = msg.arg.pagefault.address & ~(page_size() - 1);
	pr_debug("msg.arg.pagefault.address 0x%llx\n", address);

	/* Make sure to not transfer a page twice */
	list_for_each_entry(uffd_pages, &lpi->pages, list)
		if ((uffd_pages->addr == address) && (uffd_pages->flags & UFFD_FLAG_SENT))
			return 0;

	/* Now handle the pages actually requested. */
	flags = msg.arg.pagefault.flags;
	pr_debug("msg.arg.pagefault.flags 0x%llx\n", flags);

	if (msg.event != UFFD_EVENT_PAGEFAULT) {
		pr_err("unexpected msg event %u\n", msg.event);
		return -1;
	}

	ret = handle_regular_pages(lpi, dest, address);
	if (ret < 0) {
		pr_err("Error during regular page copy\n");
		return -1;
	}

	return 0;
}

static int lazy_pages_summary(struct lazy_pages_info *lpi)
{
	pr_debug("Process %d: with UFFD transferred pages: (%ld/%ld)\n",
		 lpi->pid, lpi->copied_pages, lpi->total_pages);

	if ((lpi->copied_pages != lpi->total_pages) && (lpi->total_pages > 0)) {
		pr_warn("Only %ld of %ld pages transferred via UFFD\n", lpi->copied_pages,
			lpi->total_pages);
		pr_warn("Something probably went wrong.\n");
		return 1;
	}

	return 0;
}

#define POLL_TIMEOUT 5000

static int handle_requests(int epollfd, struct epoll_event *events)
{
	int nr_fds = task_entries->nr_tasks;
	struct lazy_pages_info *lpi;
	int ret = -1;
	unsigned long ps;
	void *dest;
	int i;

	/* All operations will be done on page size */
	ps = page_size();
	dest = malloc(ps);

	while (1) {
		/*
		 * Setting the timeout to 5 seconds. If after this time
		 * no uffd pages are requested the code switches to
		 * copying the remaining pages.
		 */
		ret = epoll_wait(epollfd, events, nr_fds, POLL_TIMEOUT);
		pr_debug("epoll() ret: 0x%x\n", ret);
		if (ret < 0) {
			pr_perror("polling failed");
			goto out;
		} else if (ret == 0) {
			pr_debug("read timeout\n");
			pr_debug("switching from request to copy mode\n");
			break;
		}

		for (i = 0; i < ret; i++) {
			lpi = uffd_to_lpi(events[i].data.fd);
			ret = handle_user_fault(lpi, dest);
			if (ret < 0)
				goto out;
		}
	}
	pr_debug("Handle remaining pages\n");
	for (i = 0; i < LPI_HASH_SIZE; i++) {
		hlist_for_each_entry(lpi, &lpi_hash[i], hash) {
			ret = handle_remaining_pages(lpi, dest);
			if (ret < 0) {
				pr_err("Error during remaining page copy\n");
				ret = 1;
				goto out;
			}
		}
	}

	for (i = 0; i < LPI_HASH_SIZE; i++)
		hlist_for_each_entry(lpi, &lpi_hash[i], hash)
			ret += lazy_pages_summary(lpi);

out:
	free(dest);
	return ret;

}

static int lazy_pages_prepare_pstree(void)
{
	if (check_img_inventory() == -1)
		return -1;

	/* Allocate memory for task_entries */
	if (prepare_task_entries() == -1)
		return -1;

	if (prepare_pstree() == -1)
		return -1;

	return 0;
}

static int prepare_epoll(int nr_fds, struct epoll_event **events)
{
	int epollfd;

	*events = malloc(sizeof(struct epoll_event) * nr_fds);
	if (!*events) {
		pr_err("memory allocation failed\n");
		return -1;
	}

	epollfd = epoll_create(nr_fds);
	if (epollfd == -1) {
		pr_perror("epoll_create failed");
		goto free_events;
	}

	return epollfd;

free_events:
	free(*events);
	return -1;
}

static int epoll_add_fd(int epollfd, int fd)
{
	struct epoll_event ev;

	ev.events = EPOLLIN;
	ev.data.fd = fd;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		pr_perror("epoll_ctl failed");
		return -1;
	}

	return 0;
}

static int prepare_uffds(int epollfd)
{
	int i;
	int listen;
	struct sockaddr_un saddr;

	pr_debug("Waiting for incoming connections on %s\n", opts.addr);
	if ((listen = server_listen(&saddr)) < 0) {
		pr_perror("server_listen error");
		return -1;
	}

	for (i = 0; i < task_entries->nr_tasks; i++) {
		struct lazy_pages_info *lpi;
		lpi = ud_open(listen, &saddr);
		if (!lpi)
			goto close_uffd;
		if (epoll_add_fd(epollfd, lpi->uffd))
			goto close_uffd;
	}

	close(listen);
	return 0;

close_uffd:
	lpi_hash_fini();
	close(listen);
	return -1;
}

int cr_lazy_pages()
{
	struct epoll_event *events;
	int epollfd;
	int ret;

	if (check_for_uffd())
		return -1;

	if (!opts.addr) {
		pr_info("Please specify a file name for the unix domain socket\n");
		pr_info("used to communicate between the lazy-pages server\n");
		pr_info("and the restore process. Use the --address option like\n");
		pr_info("criu --lazy-pages --address /tmp/userfault.socket\n");
		return -1;
	}

	lpi_hash_init();

	if (lazy_pages_prepare_pstree())
		return -1;

	epollfd = prepare_epoll(task_entries->nr_tasks, &events);
	if (epollfd < 0)
		return -1;

	if (prepare_uffds(epollfd))
		return -1;

	ret = handle_requests(epollfd, events);
	lpi_hash_fini();

	return ret;
}
