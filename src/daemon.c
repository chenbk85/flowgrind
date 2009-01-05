#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <syslog.h>
#include <sys/time.h>
#include <netdb.h>
#include <pthread.h>

#include "common.h"
#include "debug.h"
#include "fg_pcap.h"
#include "fg_socket.h"
#include "fg_time.h"
#include "log.h"
#include "svnversion.h"
#include "acl.h"
#include "daemon.h"
#include "source.h"
#include "destination.h"

#ifdef HAVE_FLOAT_H
#include <float.h>
#endif

#ifdef __SOLARIS__
#define RANDOM_MAX		4294967295UL	/* 2**32-1 */
#elif __DARWIN__
#define RANDOM_MAX		LONG_MAX	/* Darwin */
#else
#define RANDOM_MAX		RAND_MAX	/* Linux, FreeBSD */
#endif

#define CONGESTION_LIMIT 10000

int daemon_pipe[2];

double reporting_interval = 0.05;

int next_flow_id = 0;

struct _timer {
	struct timeval start;
	struct timeval next;
	struct timeval last;
};
static struct _timer timer;

pthread_mutex_t mutex;
struct _request *requests = 0, *requests_last = 0;

fd_set rfds, wfds, efds;
int maxfd;

struct _report* reports = 0;
struct _report* reports_last = 0;
int pending_reports = 0;

struct _flow flows[MAX_FLOWS];
unsigned int num_flows = 0;

char started = 0;

int prepare_fds() {

	int need_timeout = 0;

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);

	FD_SET(daemon_pipe[0], &rfds);
	maxfd = daemon_pipe[0];

	need_timeout += source_prepare_fds(&rfds, &wfds, &efds, &maxfd);
	need_timeout += destination_prepare_fds(&rfds, &wfds, &efds, &maxfd);

	return need_timeout;
}

static void start_flows(struct _request_start_flows *request)
{
	int start_timestamp = request->start_timestamp;

	tsc_gettimeofday(&timer.start);
	if (timer.start.tv_sec < start_timestamp) {
		/* If the clock is syncrhonized between nodes, all nodes will start 
		   at the same time regardless of any RPC delays */
		timer.start.tv_sec = start_timestamp;
		timer.start.tv_usec = 0;
	}
	timer.last = timer.next = timer.start;
	time_add(&timer.next, reporting_interval);

	for (unsigned int i = 0; i < num_flows; i++) {
		struct _flow *flow = &flows[i];

		/* READ and WRITE */
		for (int j = 0; j < 2; j++) {
			flow->start_timestamp[j] = timer.start;
			time_add(&flow->start_timestamp[j], flow->settings.delay[j]);
			if (flow->settings.duration[j] >= 0) {
				flow->stop_timestamp[j] = flow->start_timestamp[j];
				time_add(&flow->stop_timestamp[j], flow->settings.duration[j]);
			}
		}
		if (flow->settings.write_rate)
			flow->next_write_block_timestamp = flow->start_timestamp[WRITE];

		tsc_gettimeofday(&flow->last_report_time);
	}

	started = 1;
}


static void process_requests()
{
	int rc;
	pthread_mutex_lock(&mutex);

	char tmp[100];
	for (;;) {
		int rc = read(daemon_pipe[0], tmp, 100);
		if (rc != 100)
			break;
	}

	while (requests)
	{
		struct _request* request = requests;
		requests = requests->next;
		rc = 0;

		switch (request->type) {
		case REQUEST_ADD_DESTINATION:
			add_flow_destination((struct _request_add_flow_destination *)request);
			break;
		case REQUEST_ADD_SOURCE:
			rc = add_flow_source((struct _request_add_flow_source *)request);
			break;
		case REQUEST_START_FLOWS:
			start_flows((struct _request_start_flows *)request);
			break;
		default:
			request->error = "Unknown request type";
			break;
		}
		if (rc != 1)
			pthread_cond_signal(request->condition);
	};

	pthread_mutex_unlock(&mutex);
}

static void report_flow(struct _flow* flow)
{
	struct _report* report = (struct _report*)malloc(sizeof(struct _report));
	
	report->id = flow->id;
	report->begin = flow->last_report_time;
	tsc_gettimeofday(&report->end);
	flow->last_report_time = report->end;
	report->bytes_read = flow->statistics[INTERVAL].bytes_read;
	report->bytes_written = flow->statistics[INTERVAL].bytes_written;
	report->reply_blocks_read = flow->statistics[INTERVAL].reply_blocks_read;

	report->rtt_min = flow->statistics[INTERVAL].rtt_min;
	report->rtt_max = flow->statistics[INTERVAL].rtt_max;
	report->rtt_sum = flow->statistics[INTERVAL].rtt_sum;
	report->iat_min = flow->statistics[INTERVAL].iat_min;
	report->iat_max = flow->statistics[INTERVAL].iat_max;
	report->iat_sum = flow->statistics[INTERVAL].iat_sum;

#ifdef __LINUX__
	report->tcp_info = flow->statistics[INTERVAL].tcp_info;
printf("\n====== %d ====\n", report->tcp_info.tcpi_snd_ssthresh);
#endif
	report->mss = flow->mss;
	report->mtu = flow->mtu;

	/* New report interval, reset old data */
	flow->statistics[INTERVAL].bytes_read = 0;
	flow->statistics[INTERVAL].bytes_written = 0;
	flow->statistics[INTERVAL].reply_blocks_read = 0;

	flow->statistics[INTERVAL].rtt_min = +INFINITY;
	flow->statistics[INTERVAL].rtt_max = -INFINITY;
	flow->statistics[INTERVAL].rtt_sum = 0;
	flow->statistics[INTERVAL].iat_min = +INFINITY;
	flow->statistics[INTERVAL].iat_max = -INFINITY;
	flow->statistics[INTERVAL].iat_sum = 0;

	add_report(report);
}

#ifdef __LINUX__
int get_tcp_info(struct _flow *flow, struct tcp_info *info)
{
	struct tcp_info tmp_info;
	socklen_t info_len = sizeof(tmp_info);
	int rc;

	rc = getsockopt(flow->fd, IPPROTO_TCP, TCP_INFO, &tmp_info, &info_len);
	if (rc == -1) {
		error(ERR_WARNING, "getsockopt() failed: %s",
				strerror(errno));
		return -1;
	}
	*info = tmp_info;

	return 0;
}
#endif

static void timer_check()
{
	struct timeval now;

	if (!started)
		return;

	tsc_gettimeofday(&now);
	if (time_is_after(&now, &timer.next)) {

		for (unsigned int i = 0; i < num_flows; i++) {
			struct _flow *flow = &flows[i];

#ifdef __LINUX__
			get_tcp_info(flow, &flow->statistics[INTERVAL].tcp_info);
#endif
			report_flow(flow);
		}
		timer.last = now;
		while (time_is_after(&now, &timer.next))
			time_add(&timer.next, reporting_interval);
	}
}

void* daemon_main(void* ptr __attribute__((unused)))
{
	struct timeval timeout;
	for (;;) {
		int need_timeout = prepare_fds();

		timeout.tv_sec = 0;
		timeout.tv_usec = 10000;

		int rc = select(maxfd + 1, &rfds, &wfds, &efds, need_timeout ? &timeout : 0);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			error(ERR_FATAL, "select() failed: %s",
					strerror(errno));
			exit(1);
		}

		if (FD_ISSET(daemon_pipe[0], &rfds))
			process_requests();

		timer_check();

		source_process_select(&rfds, &wfds, &efds);
		destination_process_select(&rfds, &wfds, &efds);
	}
}

void add_report(struct _report* report)
{
	pthread_mutex_lock(&mutex);

	// Do not keep too much data
	if (pending_reports >= 100) {
		free(report);
		pthread_mutex_unlock(&mutex);
		return;
	}

	report->next = 0;

	if (reports_last)
		reports_last->next = report;
	else
		reports = report;
	reports_last = report;

	pending_reports++;

	pthread_mutex_unlock(&mutex);
}

struct _report* get_reports()
{
	struct _report* ret;

	pthread_mutex_lock(&mutex);

	ret = reports;
	pending_reports = 0;
	reports = NULL;
	reports_last = 0;

	pthread_mutex_unlock(&mutex);

	return ret;
}

void init_flow(struct _flow* flow, int is_source)
{
	flow->id = next_flow_id++;
	flow->state = is_source ? WAIT_CONNECT_REPLY : WAIT_ACCEPT_REPLY;
	flow->fd = -1;
	flow->fd_reply = -1;
	flow->listenfd_reply = -1;
	flow->listenfd_data = -1;

	flow->read_block = 0;
	flow->read_block_bytes_read = 0;
	flow->read_block_count = 0;
	flow->write_block = 0;
	flow->write_block_bytes_written = 0;
	flow->write_block_count = 0;

	flow->reply_block_bytes_read = 0;

	flow->last_block_read.tv_sec = 0;
	flow->last_block_read.tv_usec = 0;

	flow->connect_called = 0;
	flow->finished[READ] = flow->finished[WRITE] = 0;

	flow->addr = 0;

	/* INTERVAL and TOTAL */
	for (int i = 0; i < 2; i++) {
		flow->statistics[i].bytes_read = 0;
		flow->statistics[i].bytes_written = 0;
		flow->statistics[i].reply_blocks_read = 0;

		flow->statistics[i].rtt_min = +INFINITY;
		flow->statistics[i].rtt_max = -INFINITY;
		flow->statistics[i].rtt_sum = 0;

		flow->statistics[i].iat_min = +INFINITY;
		flow->statistics[i].iat_max = -INFINITY;
		flow->statistics[i].iat_sum = 0;
	}

	flow->congestion_counter = 0;
}

void uninit_flow(struct _flow *flow)
{
	if (flow->fd_reply != -1)
		close(flow->fd_reply);
	if (flow->fd != -1)
		close(flow->fd);
	if (flow->listenfd_reply != -1)
		close(flow->listenfd_reply);
	if (flow->listenfd_data != -1)
		close(flow->listenfd_data);
	if (flow->read_block)
		free(flow->read_block);
	if (flow->write_block)
		free(flow->write_block);
	if (flow->addr)
		free(flow->addr);
}

void remove_flow(unsigned int i)
{
	for (unsigned int j = i; j < num_flows - 1; j++)
		flows[j] = flows[j + 1];
	num_flows--;
	if (!num_flows)
		started = 0;
}

static double flow_interpacket_delay(struct _flow *flow)
{
	double delay = 0;

	DEBUG_MSG(5, "flow %d has rate %u", flow->id, flow->settings.write_rate);
	if (flow->settings.poisson_distributed) {
		double urand = (double)((random()+1.0)/(RANDOM_MAX+1.0));
		double erand = -log(urand) * 1/(double)flow->settings.write_rate;
		delay = erand;
	} else {
		delay = (double)1/flow->settings.write_rate;
	}

	DEBUG_MSG(5, "new interpacket delay %.6f for flow %d.", delay, flow->id);
	return delay;
}

int write_data(struct _flow *flow)
{
	int rc = 0;

	/* Please note: you could argue that the following loop
	   is not necessary as not filling the socket send queue completely
	   would make the next select call return this very socket in wfds
	   and thus sending more blocks would immediately happen. However,
	   calling select with a non-full send queue might make the kernel
	   think we don't have more data to send. As a result, the kernel
	   might trigger some scheduling or whatever heuristics which would
	   not take place if we had written immediately. On the other hand,
	   in case the network is not a bottleneck the loop may take forever. */
	/* XXX: Detect this! */
	for (;;) {
		if (flow->write_block_bytes_written == 0) {
			DEBUG_MSG(5, "new write block %llu on flow %d",
					(long long unsigned int)flow->write_block_count, flow->id);
			flow->write_block[0] = sizeof(struct timeval) + 1;
			tsc_gettimeofday((struct timeval *)(flow->write_block + 1));
		}

		rc = write(flow->fd,
				flow->write_block +
				flow->write_block_bytes_written,
				flow->settings.write_block_size -
				flow->write_block_bytes_written);

		if (rc == -1) {
			if (errno == EAGAIN) {
				DEBUG_MSG(5, "write queue limit hit "
						"for flow %d", flow->id);
				break;
			}
			error(ERR_WARNING, "Premature end of test: %s",
					strerror(errno));
//xx_stop
			return -1;
		}

		if (rc == 0) {
			DEBUG_MSG(5, "flow %d sent zero bytes. what does that mean?", flow->id);
			break;
		}

		DEBUG_MSG(4, "flow %d sent %d bytes of %u (already = %u)", flow->id, rc,
				flow->settings.write_block_size,
				flow->write_block_bytes_written);

		flow->statistics[INTERVAL].bytes_written += rc;
		flow->statistics[TOTAL].bytes_written += rc;
		flow->write_block_bytes_written += rc;
		if (flow->write_block_bytes_written >=
				flow->settings.write_block_size) {
			flow->write_block_bytes_written = 0;
			tsc_gettimeofday(&flow->last_block_written);
			flow->write_block_count++;

			if (flow->settings.write_rate) {
				time_add(&flow->next_write_block_timestamp,
						flow_interpacket_delay(flow));
				if (time_is_after(&flow->last_block_written, &flow->next_write_block_timestamp)) {
					/* TODO: log time_diff and check if
					 * it's growing (queue build up) */
					DEBUG_MSG(3, "incipient congestion on "
							"flow %u (block %llu): "
							"new block scheduled "
							"for %s, %.6lfs before now.",
							flow->id,
							flow->write_block_count,
							ctime_us(&flow->next_write_block_timestamp),
							time_diff(&flow->next_write_block_timestamp, &flow->last_block_written));
					flow->congestion_counter++;
					if (flow->congestion_counter >
							CONGESTION_LIMIT &&
							flow->settings.flow_control) {
//xx_stop
						return -1;
					}
					
				}
			}
			if (flow->settings.cork && toggle_tcp_cork(flow->fd) == -1)
				DEBUG_MSG(4, "failed to recork test socket "
						"for flow %d: %s",
						flow->id, strerror(errno));
		}

		if (!flow->settings.pushy)
			break;
	}
	return 0;
}

int read_data(struct _flow *flow)
{
	int rc;
	struct iovec iov;
	struct msghdr msg;
	char cbuf[512];
	struct cmsghdr *cmsg;

	for (;;) {
		if (flow->read_block_bytes_read == 0)
			DEBUG_MSG(5, "new read block %llu on flow %d",
					(long long unsigned int)flow->read_block_count, flow->id);

		iov.iov_base = flow->read_block +
			flow->read_block_bytes_read;
		iov.iov_len = flow->settings.read_block_size -
			flow->read_block_bytes_read;
		// no name required
		msg.msg_name = NULL;
		msg.msg_namelen = 0;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
		rc = recvmsg(flow->fd, &msg, 0);

		if (rc == -1) {
			if (errno == EAGAIN)
				break;
			error(ERR_WARNING, "Premature end of test: %s",
					strerror(errno));
//xx_stop
			return -1;
		}

		if (rc == 0) {
			DEBUG_MSG(1, "server shut down test socket "
					"of flow %d", flow->id);
			if (!flow->finished[READ] ||
					!flow->settings.shutdown)
				error(ERR_WARNING, "Premature shutdown of "
						"server flow");
			flow->finished[READ] = 1;
			if (flow->finished[WRITE]) {
				DEBUG_MSG(4, "flow %u finished", flow->id);
//xx_stop
				return -1;
			}
			return 0;
		}

		DEBUG_MSG(4, "flow %d received %u bytes", flow->id, rc);

#if 0
		if (flow->settings[DESTINATION].duration[WRITE] == 0)
			error(ERR_WARNING, "flow %d got unexpected data "
					"from server (no two-way)", id);
		else if (server_flow_in_delay(id))
			error(ERR_WARNING, "flow %d got unexpected data "
					"from server (too early)", id);
		else if (!server_flow_sending(id))
			error(ERR_WARNING, "flow %d got unexpected data "
					"from server (too late)", id);
#endif

		flow->statistics[INTERVAL].bytes_read += rc;
		flow->statistics[TOTAL].bytes_read += rc;
		flow->read_block_bytes_read += rc;
		if (flow->read_block_bytes_read >= flow->settings.read_block_size) {
			assert(flow->read_block_bytes_read == flow->settings.read_block_size);

			flow->read_block_count++;
			flow->read_block_bytes_read = 0;

			int reply_block_length = flow->read_block[0] + sizeof(double);
			double *iat_ptr = (double *)(flow->read_block
				+ flow->read_block[0]);
			if (flow->settings.read_block_size >= reply_block_length) {
				if (flow->last_block_read.tv_sec == 0 &&
					flow->last_block_read.tv_usec == 0) {
					*iat_ptr = NAN;
					DEBUG_MSG(5, "isnan = %d",
						isnan(*iat_ptr));
				} else
					*iat_ptr = time_diff_now(
						&flow->last_block_read);
				tsc_gettimeofday(&flow->last_block_read);
				rc = write(flow->fd_reply, flow->read_block,
						reply_block_length);
				if (rc == -1) {
					if (errno == EAGAIN) {
						logging_log(LOG_WARNING,
							"congestion on "
							"control connection, "
							"dropping reply block");
					}
					else {
						logging_log(LOG_WARNING,
							"Premature end of test: %s",
							strerror(errno));
						return -1;
					}
				}
				else {
					DEBUG_MSG(4, "sent reply block (IAT = "
						"%.3lf)", (isnan(*iat_ptr) ?
							NAN : (*iat_ptr) * 1e3));
				}
			}
		}

		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg;
				cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			DEBUG_MSG(2, "flow %d received cmsg: type = %u, len = %u",
					flow->id, cmsg->cmsg_type, cmsg->cmsg_len);
		}

		if (!flow->settings.pushy)
			break;
	}
	return 0;
}

static void process_reply(struct _flow* flow)
{
	struct timeval now;
	/* XXX: There is actually a conversion from
		network to host byte order needed here!! */
	struct timeval *sent = (struct timeval *)(flow->reply_block + 1);
	double current_rtt;
	double *current_iat_ptr = (double *)(flow->reply_block + sizeof(struct timeval) + 1);

	tsc_gettimeofday(&now);
	current_rtt = time_diff(sent, &now);

	if ((!isnan(*current_iat_ptr) && *current_iat_ptr <= 0) || current_rtt <= 0) {
		DEBUG_MSG(5, "illegal reply_block: isnan = %d, iat = %e, rtt = %e", isnan(*current_iat_ptr), *current_iat_ptr, current_rtt);
		error(ERR_WARNING, "Found block with illegal round trip time or illegal inter arrival time, ignoring block.");
		return ;
	}

	/* Update statistics for flow, both INTERVAL and TOTAL. */
	for (int i = 0; i < 2; i++) {
		flow->statistics[i].reply_blocks_read++;

		/* Round trip times */
		ASSIGN_MIN(flow->statistics[i].rtt_min, current_rtt);
		ASSIGN_MAX(flow->statistics[i].rtt_max, current_rtt);
		flow->statistics[i].rtt_sum += current_rtt;
	
		/* Inter arrival times */
		if (!isnan(*current_iat_ptr)) {
			ASSIGN_MIN(flow->statistics[i].iat_min, *current_iat_ptr);
			ASSIGN_MAX(flow->statistics[i].iat_max, *current_iat_ptr);
			flow->statistics[i].iat_sum += *current_iat_ptr;
		}

	}
	// XXX: else: check that this only happens once!
	DEBUG_MSG(4, "processed reply_block of flow %d, (RTT = %.3lfms, IAT = %.3lfms)", flow->id, current_rtt * 1e3, isnan(*current_iat_ptr) ? NAN : *current_iat_ptr * 1e3);
}

int read_reply(struct _flow *flow)
{
	int rc = 0;

	for (;;) {
		rc = recv(flow->fd_reply,
				flow->reply_block + flow->reply_block_bytes_read,
				sizeof(flow->reply_block) -
				flow->reply_block_bytes_read, 0);
		if (rc == -1) {
			if (errno == EAGAIN)
				break;
			error(ERR_WARNING, "Premature end of test: %s",
					strerror(errno));
//xx_stop
			return -1;
		}

		if (rc == 0) {
			error(ERR_WARNING, "Premature end of test: server "
					"shut down control of flow %d.", flow->id);
//xx_stop
			return -1;
		}

		flow->reply_block_bytes_read += rc;
		if (flow->reply_block_bytes_read >=
				sizeof(flow->reply_block)) {
			process_reply(flow);
			flow->reply_block_bytes_read = 0;
		} else {
			DEBUG_MSG(4, "got partial reply_block for flow %d", flow->id);
		}

	}
	return 0;
}