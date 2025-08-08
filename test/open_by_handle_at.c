/* SPDX-License-Identifier: MIT */
/*
 * Description: run various {name_to,open_by}_handle_at(2) tests
 *
 */
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "helpers.h"
#include "liburing.h"

static int TMP_FD;

void dump_file_handle(struct file_handle *f)
{
	printf("fh len: %u, type: %d, bytes: 0x", f->handle_bytes, f->handle_type);
	for (int i = 0; i < f->handle_bytes; i++) {
		printf("%x", f->f_handle[i]);
	}
	printf("\n");
}

static bool file_handle_matches(struct file_handle *a, struct file_handle *b)
{
	if (a->handle_bytes != b->handle_bytes)
		return false;

	if (memcmp(a, b, a->handle_bytes))
		return false;

	return true;
}

static void submit_and_wait_one(struct io_uring *ring, struct io_uring_cqe **cqe)
{
	int ret;

	ret = io_uring_submit(ring);
	assert(ret == 1);

	ret = io_uring_wait_cqe(ring, cqe);
	assert(ret >= 0);

	io_uring_cqe_seen(ring, *cqe);
}

static struct io_uring_sqe *get_sqe(struct io_uring *ring)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	assert(sqe != NULL);
	return sqe;
}

static struct file_handle *get_file_handle(void)
{
	struct file_handle *h = calloc(1, sizeof *h + MAX_HANDLE_SZ);
	assert(h != NULL);
	h->handle_bytes = MAX_HANDLE_SZ;
	return h;
}

static void test_basic(struct io_uring *ring, char *path, bool direct)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe = NULL;
	int ret;
	int registered_fd = -1;
	const int fixed_fd = 0;

	if (direct) {
		ret = io_uring_register_files(ring, &registered_fd, 1);
		assert(ret == 0);
	}

	/* do name_to_handle_at() */
	sqe = get_sqe(ring);
	struct file_handle *handle = get_file_handle();
	int mount_id;
	io_uring_prep_name_to_handle_at(sqe, AT_FDCWD, path, handle, &mount_id, 0);
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == 0);

	/* do open_by_handle_at() */
	sqe = get_sqe(ring);
	if (direct) {
		io_uring_prep_open_by_handle_at_direct(sqe, TMP_FD, handle, O_RDWR, fixed_fd);
	} else {
		io_uring_prep_open_by_handle_at(sqe, TMP_FD, handle, O_RDWR);
	}
	submit_and_wait_one(ring, &cqe);

	if (direct)
		assert(cqe->res == 0);
	else
		assert(cqe->res >= 0);

	/* Test read and write: */

	/* Choose the FD to read/write to based on direct or not: */
	int fd = direct ? fixed_fd : cqe->res;
	const char pattern = 0x42;

	sqe = get_sqe(ring);
	io_uring_prep_write(sqe, fd, &pattern, sizeof(pattern), 0);
	if (direct)
		sqe->flags |= IOSQE_FIXED_FILE;
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == 1);

	/* try a read */
	char pattern_out = 0;

	sqe = get_sqe(ring);
	io_uring_prep_read(sqe, fd, &pattern_out, sizeof(pattern_out), 0);
	if (direct)
		sqe->flags |= IOSQE_FIXED_FILE;
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == 1);

	assert(pattern == pattern_out);

	/* Close it: */
	sqe = get_sqe(ring);
	if (direct)
		io_uring_prep_close_direct(sqe, fd);
	else
		io_uring_prep_close(sqe, fd);
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == 0);

	/* now an attempt to do something with the FD should fail: */
	sqe = get_sqe(ring);
	io_uring_prep_read(sqe, fd, &pattern_out, sizeof(pattern_out), 0);
	if (direct)
		sqe->flags |= IOSQE_FIXED_FILE;
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == -EBADF);

	if (direct)
		assert(io_uring_unregister_files(ring) == 0);
}

static void test_errors_name_to_handle_at(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe = NULL;

	struct file_handle *handle = get_file_handle();
	char *path = "/tmp";
	int mount_id;
	struct file_handle *bad_handle = (void *) 0x1234;
	char *bad_path = (void *) 0x1234;
	int *bad_mount_id = (void *) 0x1234;

	sqe = get_sqe(ring);
	io_uring_prep_name_to_handle_at(sqe, AT_FDCWD, path, bad_handle, &mount_id, 0);
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == -EFAULT);

	sqe = get_sqe(ring);
	io_uring_prep_name_to_handle_at(sqe, AT_FDCWD, bad_path, handle, &mount_id, 0);
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == -EFAULT);

	sqe = get_sqe(ring);
	io_uring_prep_name_to_handle_at(sqe, AT_FDCWD, path, handle, bad_mount_id, 0);
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == -EFAULT);

	/* Bad flag: name_to_handle_at() should reject AT_* flags that it doesn't use: */
	sqe = get_sqe(ring);
	io_uring_prep_name_to_handle_at(sqe, AT_FDCWD, path, handle, &mount_id, AT_RECURSIVE);
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == -EINVAL);

	free(handle);
}

static void test_errors_open_by_handle_at(struct io_uring *ring, struct file_handle *handle)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe = NULL;

	int bad_fd = -1;

	sqe = get_sqe(ring);
	io_uring_prep_open_by_handle_at(sqe, bad_fd, handle, 0);
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == -EBADF);

	sqe = get_sqe(ring);
	io_uring_prep_open_by_handle_at(sqe, STDOUT_FILENO, handle, 0);
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == -ESTALE);

	free(handle);
}

static void test_symlink(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe = NULL;
	int ret;

	char *target = "/tmp/open_by_handle_at_target";
	char *link = "/tmp/open_by_handle_at_link";

	t_create_file(target, 4096);
	ret = symlink(target, link);
	if (ret)
		assert(errno == EEXIST);

	struct file_handle *target_fh = get_file_handle();
	struct file_handle *link_fh = get_file_handle();
	struct file_handle *uring_fh;

	int mount_id;
	ret = name_to_handle_at(AT_FDCWD, link, target_fh, &mount_id, AT_SYMLINK_FOLLOW);
	assert(ret == 0);
	ret = name_to_handle_at(AT_FDCWD, link, link_fh, &mount_id, 0);
	assert(ret == 0);

	/* confirm that io_uring version correctly follows symlink when requested: */
	uring_fh = get_file_handle();
	sqe = get_sqe(ring);
	io_uring_prep_name_to_handle_at(sqe, AT_FDCWD, link, uring_fh, &mount_id, AT_SYMLINK_FOLLOW);
	submit_and_wait_one(ring, &cqe);
	assert(file_handle_matches(target_fh, uring_fh));
	free(uring_fh);

	/* confirm that io_uring version does not follow symlink when not requested: */
	uring_fh = get_file_handle();
	sqe = get_sqe(ring);
	io_uring_prep_name_to_handle_at(sqe, AT_FDCWD, link, uring_fh, &mount_id, 0);
	submit_and_wait_one(ring, &cqe);
	assert(file_handle_matches(link_fh, uring_fh));
	free(uring_fh);

	/* confirm that open_by_handle_at() of a symlink fails unless O_PATH specified: */
	sqe = get_sqe(ring);
	io_uring_prep_open_by_handle_at(sqe, TMP_FD, link_fh, 0);
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == -ELOOP);

	sqe = get_sqe(ring);
	io_uring_prep_open_by_handle_at(sqe, TMP_FD, link_fh, O_PATH);
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res >= 0);

	free(target_fh);
	free(link_fh);
}

static void test_non_cached_open(struct io_uring *ring, char *path)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe = NULL;
	int ret;

	/* do name_to_handle_at() */
	sqe = get_sqe(ring);
	struct file_handle *handle = get_file_handle();
	int mount_id;
	io_uring_prep_name_to_handle_at(sqe, AT_FDCWD, path, handle, &mount_id, 0);
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == 0);

	/* drop dentry cache */
	int cache_fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
	assert(cache_fd >= 0);
	char data = '2';
	ret = write(cache_fd, &data, sizeof data);
	assert(ret == 1);

	/* do open_by_handle_at() */
	sqe = get_sqe(ring);
	io_uring_prep_open_by_handle_at(sqe, TMP_FD, handle, O_RDWR);
	submit_and_wait_one(ring, &cqe);

	assert(cqe->res >= 0);

	/* Close it: */
	sqe = get_sqe(ring);
	io_uring_prep_close(sqe, cqe->res);
	submit_and_wait_one(ring, &cqe);
	assert(cqe->res == 0);
}

void *stress_test_main(void *arg)
{
	struct io_uring ring;
	__u64 id = (__u64) arg;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;

	int ret = io_uring_queue_init(8, &ring, 0);
	if (ret != 0) {
		printf("io_uring_queue_init: %d\n", ret);
		goto err;
	}

	char path[32];
	sprintf(path, "/tmp/file_%lld", id);
	t_create_file(path, 4096);

	for (int i = 0; i < 1024; i++) {
		sqe = get_sqe(&ring);
		struct file_handle *handle = get_file_handle();
		int mount_id;

		/* get handle */
		io_uring_prep_name_to_handle_at(sqe, AT_FDCWD, path, handle, &mount_id, 0);
		submit_and_wait_one(&ring, &cqe);
		if (cqe->res != 0) {
			fprintf(stderr, "name_to_handle_at returned %d\n", cqe->res);
			goto err;
		}

		/* open by handle */
		sqe = get_sqe(&ring);
		io_uring_prep_open_by_handle_at(sqe, TMP_FD, handle, O_RDWR);
		submit_and_wait_one(&ring, &cqe);
		if (cqe->res < 0) {
			fprintf(stderr, "open result: %d\n", cqe->res);
			goto err;
		}

		/* read and write */
		int fd = cqe->res;
		int data = i;
		int out;
		int amount;

		amount = pwrite(fd, &data, sizeof(data), 0);
		if (amount != sizeof(data)) {
			fprintf(stderr, "Error: got %d from pwrite, expected %ld\n", amount, sizeof(data));
			goto err;
		}
		amount = pread(fd, &out, sizeof(out), 0);
		if (amount != sizeof(out)) {
			fprintf(stderr, "Error: got %d from pwrite, expected %ld\n", amount, sizeof(data));
			goto err;
		}

		assert(out == data);

		/* close */
		sqe = get_sqe(&ring);
		io_uring_prep_close(sqe, fd);
		submit_and_wait_one(&ring, &cqe);
		assert(cqe->res == 0);
	}

	return NULL;

err:
	return (void *) 1;
}

#define N_THREADS 10

void stress_test(struct io_uring *ring)
{
	pthread_t threads[N_THREADS];
	int ret;

	for (__u64 i = 0; i < N_THREADS; i++) {
		ret = pthread_create(&threads[i], NULL, stress_test_main, (void *) i);
		assert(ret == 0);
	}

	void *thread_ret;
	for (int i = 0; i < N_THREADS; i++) {
		ret = pthread_join(threads[i], &thread_ret);
		assert(ret == 0);
		assert(thread_ret == NULL);
	}
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	char *path;
	int ret;

	TMP_FD = open("/tmp", O_DIRECTORY);
	assert(TMP_FD >= 0);

	ret = io_uring_queue_init(8, &ring, 0);
	assert(ret == 0);

	path = "/tmp/open_by_handle_at.tmp";
	t_create_file(path, 4096);
	struct file_handle *handle = get_file_handle();
	int mount_id;
	name_to_handle_at(AT_FDCWD, path, handle, &mount_id, 0);

	test_basic(&ring, path, false);
	test_basic(&ring, path, true);

	test_errors_name_to_handle_at(&ring);
	test_errors_open_by_handle_at(&ring, handle);

	test_symlink(&ring);

	test_non_cached_open(&ring, path);

	stress_test(&ring);

	io_uring_queue_exit(&ring);
	close(TMP_FD);

	return 0;
}
