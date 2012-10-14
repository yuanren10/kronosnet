#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>

#include "libknet-private.h"
#include "crypto.h"

#define KNET_MAX_EVENTS 8
#define KNET_PING_TIMERES 200000

static void *_handle_tap_to_links_thread(void *data);
static void *_handle_recv_from_links_thread(void *data);
static void *_handle_heartbt_thread(void *data);
static void *_handle_dst_link_handler_thread(void *data);

knet_handle_t knet_handle_new(const struct knet_handle_cfg *knet_handle_cfg)
{
	knet_handle_t knet_h;
	struct epoll_event ev;

	/*
	 * validate incoming config request
	 */
	if (knet_handle_cfg == NULL) {
		errno = EINVAL;
		return NULL;
	}

	if (knet_handle_cfg->fd <= 0) {
		errno = EINVAL;
		return NULL;
	}

	if ((knet_h = malloc(sizeof(struct knet_handle))) == NULL)
		return NULL;

	memset(knet_h, 0, sizeof(struct knet_handle));

	knet_h->node_id = knet_handle_cfg->node_id;
	knet_h->sockfd = knet_handle_cfg->fd;

	if (pipe(knet_h->pipefd))
		goto exit_fail1;

	if ((_fdset_cloexec(knet_h->pipefd[0])) ||
	    (_fdset_cloexec(knet_h->pipefd[1])) ||
	    (_fdset_nonblock(knet_h->pipefd[0])) ||
	    (_fdset_nonblock(knet_h->pipefd[1])))
		goto exit_fail2;

	knet_h->dst_host_filter = knet_handle_cfg->dst_host_filter;
	knet_h->dst_host_filter_fn = knet_handle_cfg->dst_host_filter_fn;

	if ((knet_h->dst_host_filter) && (!knet_h->dst_host_filter_fn))
		goto exit_fail2;

	if ((knet_h->tap_to_links_buf = malloc(KNET_DATABUFSIZE))== NULL)
		goto exit_fail2;

	memset(knet_h->tap_to_links_buf, 0, KNET_DATABUFSIZE);

	if ((knet_h->recv_from_links_buf = malloc(KNET_DATABUFSIZE))== NULL)
		goto exit_fail3;

	memset(knet_h->recv_from_links_buf, 0, KNET_DATABUFSIZE);

	if ((knet_h->pingbuf = malloc(KNET_PINGBUFSIZE))== NULL)
		goto exit_fail4;

	memset(knet_h->pingbuf, 0, KNET_PINGBUFSIZE);

	if (pthread_rwlock_init(&knet_h->list_rwlock, NULL) != 0)
		goto exit_fail5;

	knet_h->tap_to_links_epollfd = epoll_create(KNET_MAX_EVENTS);
	knet_h->recv_from_links_epollfd = epoll_create(KNET_MAX_EVENTS);
	knet_h->dst_link_handler_epollfd = epoll_create(KNET_MAX_EVENTS);

	if ((knet_h->tap_to_links_epollfd < 0) ||
	    (knet_h->recv_from_links_epollfd < 0) ||
	    (knet_h->dst_link_handler_epollfd < 0))
		goto exit_fail6;

	if ((_fdset_cloexec(knet_h->tap_to_links_epollfd) != 0) ||
	    (_fdset_cloexec(knet_h->recv_from_links_epollfd) != 0) ||
	    (_fdset_cloexec(knet_h->dst_link_handler_epollfd) != 0))
		goto exit_fail6;

	memset(&ev, 0, sizeof(struct epoll_event));

	ev.events = EPOLLIN;
	ev.data.fd = knet_h->sockfd;

	if (epoll_ctl(knet_h->tap_to_links_epollfd,
				EPOLL_CTL_ADD, knet_h->sockfd, &ev) != 0)
		goto exit_fail6;

	memset(&ev, 0, sizeof(struct epoll_event));

	ev.events = EPOLLIN;
	ev.data.fd = knet_h->pipefd[0];

	if (epoll_ctl(knet_h->dst_link_handler_epollfd,
				EPOLL_CTL_ADD, knet_h->pipefd[0], &ev) != 0)
		goto exit_fail6;

	if (pthread_create(&knet_h->dst_link_handler_thread, 0,
				_handle_dst_link_handler_thread, (void *) knet_h) != 0)
		goto exit_fail6;

	if (pthread_create(&knet_h->tap_to_links_thread, 0,
				_handle_tap_to_links_thread, (void *) knet_h) != 0)
		goto exit_fail7;

	if (pthread_create(&knet_h->recv_from_links_thread, 0,
				_handle_recv_from_links_thread, (void *) knet_h) != 0)
		goto exit_fail8;

	if (pthread_create(&knet_h->heartbt_thread, 0,
				_handle_heartbt_thread, (void *) knet_h) != 0)
		goto exit_fail9;

	return knet_h;

exit_fail9:
	pthread_cancel(knet_h->recv_from_links_thread);

exit_fail8:
	pthread_cancel(knet_h->tap_to_links_thread);

exit_fail7:
	pthread_cancel(knet_h->dst_link_handler_thread);

exit_fail6:
	if (knet_h->tap_to_links_epollfd >= 0)
		close(knet_h->tap_to_links_epollfd);
	if (knet_h->recv_from_links_epollfd >= 0)
		close(knet_h->recv_from_links_epollfd);
	if (knet_h->dst_link_handler_epollfd >= 0)
		close(knet_h->dst_link_handler_epollfd);

	pthread_rwlock_destroy(&knet_h->list_rwlock);

exit_fail5:
	free(knet_h->pingbuf);

exit_fail4:
	free(knet_h->recv_from_links_buf);

exit_fail3:
	free(knet_h->tap_to_links_buf);

exit_fail2:
	close(knet_h->pipefd[0]);
	close(knet_h->pipefd[1]);

exit_fail1:
	free(knet_h);
	return NULL;
}

int knet_handle_free(knet_handle_t knet_h)
{
	void *retval;

	if ((knet_h->host_head != NULL) || (knet_h->listener_head != NULL))
		goto exit_busy;

	pthread_cancel(knet_h->heartbt_thread);
	pthread_join(knet_h->heartbt_thread, &retval);

	if (retval != PTHREAD_CANCELED)
		goto exit_busy;

	pthread_cancel(knet_h->tap_to_links_thread);
	pthread_join(knet_h->tap_to_links_thread, &retval);
	if (retval != PTHREAD_CANCELED)
		goto exit_busy;

	pthread_cancel(knet_h->recv_from_links_thread);
	pthread_join(knet_h->recv_from_links_thread, &retval);

	if (retval != PTHREAD_CANCELED)
		goto exit_busy;

	pthread_cancel(knet_h->dst_link_handler_thread);
	pthread_join(knet_h->dst_link_handler_thread, &retval);

	if (retval != PTHREAD_CANCELED)
		goto exit_busy;

	close(knet_h->tap_to_links_epollfd);
	close(knet_h->recv_from_links_epollfd);
	close(knet_h->dst_link_handler_epollfd);
	close(knet_h->pipefd[0]);
	close(knet_h->pipefd[1]);

	pthread_rwlock_destroy(&knet_h->list_rwlock);

	free(knet_h->tap_to_links_buf);
	free(knet_h->recv_from_links_buf);
	free(knet_h->pingbuf);

	crypto_fini(knet_h);

	free(knet_h);

	return 0;

 exit_busy:
	errno = EBUSY;
	return -EBUSY;
}

void knet_handle_setfwd(knet_handle_t knet_h, int enabled)
{
	knet_h->enabled = (enabled == 1) ? 1 : 0;
}

int knet_handle_crypto(knet_handle_t knet_h, struct knet_handle_crypto_cfg *knet_handle_crypto_cfg)
{
	if (knet_h->enabled)
		return -1;

	return crypto_init(knet_h, knet_handle_crypto_cfg);
}

int knet_link_enable(knet_handle_t knet_h, uint16_t node_id, struct knet_link *lnk, int configured)
{
	int write_retry = 0;

	if (lnk->configured == configured)
		return 0;

	lnk->configured = configured;

	if (configured)
		return 0;

try_again:
	if (write(knet_h->pipefd[1], &node_id, sizeof(node_id)) != sizeof(node_id)) {
		if ((write_retry < 10) && ((errno = EAGAIN) || (errno = EWOULDBLOCK))) {
			write_retry++;
			goto try_again;
		} else {
			return -1;
		}
	}
	return 0;
}

void knet_link_timeout(struct knet_link *lnk,
				time_t interval, time_t timeout, int precision)
{
	lnk->ping_interval = interval * 1000; /* microseconds */
	lnk->pong_timeout = timeout * 1000; /* microseconds */
	lnk->latency_fix = precision;
	lnk->latency_exp = precision - \
				((lnk->ping_interval * precision) / 8000000);
}

static void _handle_tap_to_links(knet_handle_t knet_h)
{
	ssize_t inlen, len, outlen;
	struct knet_host *dst_host;
	int link_idx;
	uint16_t dst_host_ids[KNET_MAX_HOST];
	size_t dst_host_ids_entries = 0;
	int bcast = 1;
	unsigned char *outbuf = (unsigned char *)knet_h->tap_to_links_buf;

	inlen = read(knet_h->sockfd, knet_h->tap_to_links_buf->kf_data,
					KNET_DATABUFSIZE - (KNET_FRAME_SIZE + sizeof(seq_num_t)));

	if (inlen == 0) {
		/* TODO: disconnection, should never happen! */
		return;
	}

	outlen = len = inlen + KNET_FRAME_SIZE + sizeof(seq_num_t);

	if (knet_h->enabled != 1) /* data forward is disabled */
		return;

	if (knet_h->dst_host_filter) {
		bcast = knet_h->dst_host_filter_fn(
				(const unsigned char *)knet_h->tap_to_links_buf->kf_data,
				inlen,
				knet_h->tap_to_links_buf->kf_node,
				dst_host_ids,
				&dst_host_ids_entries);
		if (bcast < 0)
			return;

		if ((!bcast) && (!dst_host_ids_entries))
			return;
	}

	if (pthread_rwlock_rdlock(&knet_h->list_rwlock) != 0)
		return;

	if (!bcast) {
		int host_idx;

		for (host_idx = 0; host_idx < dst_host_ids_entries; host_idx++) {
			dst_host = knet_h->host_index[dst_host_ids[host_idx]];
			if (!dst_host)
				continue;

			knet_h->tap_to_links_buf->kf_seq_num = htons(++dst_host->ucast_seq_num_tx);

			if (knet_h->crypto_instance) {
				if (crypto_encrypt_and_sign(knet_h->crypto_instance,
						    (const unsigned char *)knet_h->tap_to_links_buf,
						    len,
						    knet_h->tap_to_links_buf_crypt,
						    &outlen) < 0) {
					pthread_rwlock_unlock(&knet_h->list_rwlock);
					return;
				}
				outbuf = knet_h->tap_to_links_buf_crypt;
			}

			for (link_idx = 0; link_idx < dst_host->active_link_entries; link_idx++) {
				sendto(dst_host->link[dst_host->active_links[link_idx]].sock,
						outbuf, outlen, MSG_DONTWAIT,
						(struct sockaddr *) &dst_host->link[dst_host->active_links[link_idx]].address,
						sizeof(struct sockaddr_storage));

				if ((dst_host->link_handler_policy == KNET_LINK_POLICY_RR) &&
				    (dst_host->active_link_entries > 1)) {
					uint8_t cur_link_id = dst_host->active_links[0];

					memmove(&dst_host->active_links[0], &dst_host->active_links[1], KNET_MAX_LINK - 1);
					dst_host->active_links[dst_host->active_link_entries - 1] = cur_link_id;

					break;
				}
			}
		}
	} else {
		knet_h->tap_to_links_buf->kf_seq_num = htons(++knet_h->bcast_seq_num_tx);

		if (knet_h->crypto_instance) {
			if (crypto_encrypt_and_sign(knet_h->crypto_instance,
					    (const unsigned char *)knet_h->tap_to_links_buf,
					    len,
					    knet_h->tap_to_links_buf_crypt,
					    &outlen) < 0) {
				pthread_rwlock_unlock(&knet_h->list_rwlock);
				return;
			}
			outbuf = knet_h->tap_to_links_buf_crypt;
		}

		for (dst_host = knet_h->host_head; dst_host != NULL; dst_host = dst_host->next) {
			for (link_idx = 0; link_idx < dst_host->active_link_entries; link_idx++) {
				sendto(dst_host->link[dst_host->active_links[link_idx]].sock,
					outbuf, outlen, MSG_DONTWAIT,
					(struct sockaddr *) &dst_host->link[dst_host->active_links[link_idx]].address,
					sizeof(struct sockaddr_storage));

				if ((dst_host->link_handler_policy == KNET_LINK_POLICY_RR) &&
				    (dst_host->active_link_entries > 1)) {
					uint8_t cur_link_id = dst_host->active_links[0];

					memmove(&dst_host->active_links[0], &dst_host->active_links[1], KNET_MAX_LINK - 1);
					dst_host->active_links[dst_host->active_link_entries - 1] = cur_link_id;

					break;
				}
			}
		}
	}
	pthread_rwlock_unlock(&knet_h->list_rwlock);
}

static void _handle_recv_from_links(knet_handle_t knet_h, int sockfd)
{
	ssize_t len, outlen;
	struct sockaddr_storage address;
	socklen_t addrlen;
	struct knet_host *src_host;
	struct knet_link *src_link;
	unsigned long long latency_last;
	uint16_t dst_host_ids[KNET_MAX_HOST];
	size_t dst_host_ids_entries = 0;
	int bcast = 1;
	unsigned char *outbuf = (unsigned char *)knet_h->recv_from_links_buf;

	if (pthread_rwlock_rdlock(&knet_h->list_rwlock) != 0)
		return;

	addrlen = sizeof(struct sockaddr_storage);
	len = recvfrom(sockfd, knet_h->recv_from_links_buf, KNET_DATABUFSIZE,
		MSG_DONTWAIT, (struct sockaddr *) &address, &addrlen);

	if (knet_h->crypto_instance) {
		if (crypto_authenticate_and_decrypt(knet_h->crypto_instance,
						    (unsigned char *)knet_h->recv_from_links_buf,
						    &len) < 0)
			goto exit_unlock;
	}

	if (len < (KNET_FRAME_SIZE + 1))
		goto exit_unlock;

	if (ntohl(knet_h->recv_from_links_buf->kf_magic) != KNET_FRAME_MAGIC)
		goto exit_unlock;

	if (knet_h->recv_from_links_buf->kf_version != KNET_FRAME_VERSION)
		goto exit_unlock;

	knet_h->recv_from_links_buf->kf_node = ntohs(knet_h->recv_from_links_buf->kf_node);
	src_host = knet_h->host_index[knet_h->recv_from_links_buf->kf_node];
	if (src_host == NULL) {  /* host not found */
		goto exit_unlock;
	}

	src_link = NULL;

	if ((knet_h->recv_from_links_buf->kf_type & KNET_FRAME_PMSK) != 0) {
		src_link = src_host->link +
				(knet_h->recv_from_links_buf->kf_link % KNET_MAX_LINK);
	}

	switch (knet_h->recv_from_links_buf->kf_type) {
	case KNET_FRAME_DATA:
		if (knet_h->enabled != 1) /* data forward is disabled */
			break;

		knet_h->recv_from_links_buf->kf_seq_num = ntohs(knet_h->recv_from_links_buf->kf_seq_num);

		if (knet_h->dst_host_filter) {
			int host_idx;
			int found = 0;

			bcast = knet_h->dst_host_filter_fn(
					(const unsigned char *)knet_h->recv_from_links_buf->kf_data,
					len,
					knet_h->recv_from_links_buf->kf_node,
					dst_host_ids,
					&dst_host_ids_entries);
			if (bcast < 0)
				goto exit_unlock;

			if ((!bcast) && (!dst_host_ids_entries))
				goto exit_unlock;

			/* check if we are dst for this packet */
			if (!bcast) {
				for (host_idx = 0; host_idx < dst_host_ids_entries; host_idx++) {
					if (dst_host_ids[host_idx] == knet_h->node_id) {
						found = 1;
						break;
					}
				}
				if (!found)
					goto exit_unlock;
			}
		}

		if (!knet_should_deliver(src_host, bcast, knet_h->recv_from_links_buf->kf_seq_num))
			goto exit_unlock;

		if (write(knet_h->sockfd,
			  knet_h->recv_from_links_buf->kf_data,
			  len - (KNET_FRAME_SIZE + sizeof(seq_num_t))) == len - (KNET_FRAME_SIZE + sizeof(seq_num_t)))
			knet_has_been_delivered(src_host, bcast, knet_h->recv_from_links_buf->kf_seq_num);

		break;
	case KNET_FRAME_PING:
		outlen = KNET_PINGBUFSIZE;
		knet_h->recv_from_links_buf->kf_type = KNET_FRAME_PONG;
		knet_h->recv_from_links_buf->kf_node = htons(knet_h->node_id);

		if (knet_h->crypto_instance) {
			if (crypto_encrypt_and_sign(knet_h->crypto_instance,
						    (const unsigned char *)knet_h->recv_from_links_buf,
						    len,
						    knet_h->recv_from_links_buf_crypt,
						    &outlen) < 0)
				break;

			outbuf = knet_h->recv_from_links_buf_crypt;
		}

		sendto(src_link->sock, outbuf, outlen, MSG_DONTWAIT,
				(struct sockaddr *) &src_link->address,
				sizeof(struct sockaddr_storage));

		break;
	case KNET_FRAME_PONG:
		clock_gettime(CLOCK_MONOTONIC, &src_link->pong_last);

		timespec_diff(knet_h->recv_from_links_buf->kf_time,
				src_link->pong_last, &latency_last);

		src_link->latency =
			((src_link->latency * src_link->latency_exp) +
			((latency_last / 1000llu) *
				(src_link->latency_fix - src_link->latency_exp))) /
					src_link->latency_fix;

		if (src_link->latency < src_link->pong_timeout) {
			if (!src_link->connected) {
				int write_retry = 0;

				src_link->connected = 1;
try_again:
				if (write(knet_h->pipefd[1], &src_host->node_id,
					  sizeof(src_host->node_id)) != sizeof(src_host->node_id)) {
					if ((write_retry < 10) && ((errno = EAGAIN) || (errno = EWOULDBLOCK))) {
						write_retry++;
						goto try_again;
					} /* define what to do if we can't add a link */
				}
			}
		}

		break;
	default:
		goto exit_unlock;
	}

 exit_unlock:
	pthread_rwlock_unlock(&knet_h->list_rwlock);
}

static void _handle_dst_link_updates(knet_handle_t knet_h)
{
	uint16_t dst_host_id;
	struct knet_host *dst_host;
	int link_idx;
	int best_priority = -1;

	if (read(knet_h->pipefd[0], &dst_host_id, sizeof(dst_host_id)) != sizeof(dst_host_id))
		return;

	if (pthread_rwlock_wrlock(&knet_h->list_rwlock) != 0)
		return;

	dst_host = knet_h->host_index[dst_host_id];
	if (!dst_host)
		goto out_unlock;

	dst_host->active_link_entries = 0;

	for (link_idx = 0; link_idx < KNET_MAX_LINK; link_idx++) {
		if (dst_host->link[link_idx].configured != 1) /* link is not configured */
			continue;
		if (dst_host->link[link_idx].connected != 1) /* link is not enabled */
			continue;

		if (dst_host->link_handler_policy == KNET_LINK_POLICY_PASSIVE) {
			/* for passive we look for the only active link with higher priority */
			if (dst_host->link[link_idx].priority > best_priority) {
				dst_host->active_links[0] = link_idx;
				best_priority = dst_host->link[link_idx].priority;
			}
			dst_host->active_link_entries = 1;
		} else {
			/* for RR and ACTIVE we need to copy all available links */
			dst_host->active_links[dst_host->active_link_entries] = link_idx;
			dst_host->active_link_entries++;
		}
	}

	/* no active links, we can clean the circular buffers and indexes */
	if (!dst_host->active_link_entries) {
		memset(dst_host->bcast_circular_buffer, 0, KNET_CBUFFER_SIZE);
		memset(dst_host->ucast_circular_buffer, 0, KNET_CBUFFER_SIZE);
		dst_host->bcast_seq_num_rx = 0;
		dst_host->ucast_seq_num_rx = 0;
	}

out_unlock:
	pthread_rwlock_unlock(&knet_h->list_rwlock);

	return;
}

static void _handle_check_each(knet_handle_t knet_h, struct knet_host *dst_host, struct knet_link *dst_link)
{
	int len;
	ssize_t outlen = KNET_PINGBUFSIZE;
	struct timespec clock_now, pong_last;
	unsigned long long diff_ping;
	unsigned char *outbuf = (unsigned char *)knet_h->pingbuf;

	/* caching last pong to avoid race conditions */
	pong_last = dst_link->pong_last;

	if (clock_gettime(CLOCK_MONOTONIC, &clock_now) != 0)
		return;

	timespec_diff(dst_link->ping_last, clock_now, &diff_ping);

	if (diff_ping >= (dst_link->ping_interval * 1000llu)) {
		knet_h->pingbuf->kf_time = clock_now;
		knet_h->pingbuf->kf_link = dst_link->link_id;

		if (knet_h->crypto_instance) {
			if (crypto_encrypt_and_sign(knet_h->crypto_instance,
						    (const unsigned char *)knet_h->pingbuf,
						    KNET_PINGBUFSIZE,
						    knet_h->pingbuf_crypt,
						    &outlen) < 0)
				return;

			outbuf = knet_h->pingbuf_crypt;
		}

		len = sendto(dst_link->sock, outbuf, outlen,
			MSG_DONTWAIT, (struct sockaddr *) &dst_link->address,
			sizeof(struct sockaddr_storage));

		if (len == outlen)
			dst_link->ping_last = clock_now;
	}

	if (dst_link->connected == 1) {
		timespec_diff(pong_last, clock_now, &diff_ping);

		if (diff_ping >= (dst_link->pong_timeout * 1000llu)) {
			int write_retry = 0;

			dst_link->connected = 0;
try_again:
			if (write(knet_h->pipefd[1], &dst_host->node_id, sizeof(dst_host->node_id)) != sizeof(dst_host->node_id)) {
				if ((write_retry < 10) && ((errno = EAGAIN) || (errno = EWOULDBLOCK))) {
					write_retry++;
					goto try_again;
				} /* define what to do if we can't deactivate a link */
			} 
		}
	}
}

static void *_handle_heartbt_thread(void *data)
{
	knet_handle_t knet_h;
	struct knet_host *dst_host;
	int link_idx;

	knet_h = (knet_handle_t) data;

	/* preparing ping buffer */
	knet_h->pingbuf->kf_magic = htonl(KNET_FRAME_MAGIC);
	knet_h->pingbuf->kf_version = KNET_FRAME_VERSION;
	knet_h->pingbuf->kf_type = KNET_FRAME_PING;
	knet_h->pingbuf->kf_node = htons(knet_h->node_id);

	while (1) {
		usleep(KNET_PING_TIMERES);

		if (pthread_rwlock_rdlock(&knet_h->list_rwlock) != 0)
			continue;

		for (dst_host = knet_h->host_head; dst_host != NULL; dst_host = dst_host->next) {
			for (link_idx = 0; link_idx < KNET_MAX_LINK; link_idx++) {
				if (dst_host->link[link_idx].configured != 1)
					continue;
				_handle_check_each(knet_h, dst_host, &dst_host->link[link_idx]);
			}
		}

		pthread_rwlock_unlock(&knet_h->list_rwlock);
	}

	return NULL;
}

static void *_handle_tap_to_links_thread(void *data)
{
	knet_handle_t knet_h;
	struct epoll_event events[KNET_MAX_EVENTS];

	knet_h = (knet_handle_t) data;

	/* preparing data buffer */
	knet_h->tap_to_links_buf->kf_magic = htonl(KNET_FRAME_MAGIC);
	knet_h->tap_to_links_buf->kf_version = KNET_FRAME_VERSION;
	knet_h->tap_to_links_buf->kf_type = KNET_FRAME_DATA;
	knet_h->tap_to_links_buf->kf_node = htons(knet_h->node_id);

	while (1) {
		if (epoll_wait(knet_h->tap_to_links_epollfd, events, KNET_MAX_EVENTS, -1) >= 1)
			_handle_tap_to_links(knet_h);
	}

	return NULL;

}

static void *_handle_recv_from_links_thread(void *data)
{
	int i, nev;
	knet_handle_t knet_h = (knet_handle_t) data;
	struct epoll_event events[KNET_MAX_EVENTS];

	while (1) {
		nev = epoll_wait(knet_h->recv_from_links_epollfd, events, KNET_MAX_EVENTS, -1);

		for (i = 0; i < nev; i++) {
			_handle_recv_from_links(knet_h, events[i].data.fd);
		}
	}

	return NULL;
}

static void *_handle_dst_link_handler_thread(void *data)
{
	knet_handle_t knet_h = (knet_handle_t) data;
	struct epoll_event events[KNET_MAX_EVENTS];

	while (1) {
		if (epoll_wait(knet_h->dst_link_handler_epollfd, events, KNET_MAX_EVENTS, -1) >= 1)
			_handle_dst_link_updates(knet_h);
	}

	return NULL;	
}
