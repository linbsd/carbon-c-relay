/*
 *  This file is part of carbon-c-relay.
 *
 *  carbon-c-relay is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  carbon-c-relay is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with carbon-c-relay.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "relay.h"
#include "dispatcher.h"
#include "aggregator.h"

static FILE *metricsock = NULL;
static pthread_t aggregatorid;
static aggregator *aggregators = NULL;
static aggregator *lastaggr = NULL;


/**
 * Allocates a new aggregator setup to hold buckets matching interval
 * and expiry time.
 */
aggregator *
aggregator_new(unsigned int interval, unsigned int expire)
{
	int i;
	time_t now;
	aggregator *ret = malloc(sizeof(aggregator));

	if (ret == NULL)
		return ret;

	if (aggregators == NULL) {
		aggregators = lastaggr = ret;
	} else {
		lastaggr = lastaggr->next = ret;
	}

	ret->interval = interval;
	ret->expire = expire;
	ret->received = 0;
	ret->sent = 0;
	ret->dropped = 0;
	ret->next = NULL;

	pthread_mutex_init(&ret->bucketlock, NULL);

	/* start buckets in the past, but before expiry */
	time(&now);
	now -= ((expire - 1) / interval) * interval;

	/* allocate enough buckets to hold the past + some future */
	ret->bucketcnt = expire / interval + 1 + 2;
	ret->buckets = malloc(sizeof(struct _bucket *) * ret->bucketcnt);
	for (i = 0; i < ret->bucketcnt; i++) {
		ret->buckets[i] = malloc(sizeof(struct _bucket));
		ret->buckets[i]->start = now + (i * interval);
		ret->buckets[i]->cnt = 0;
	}

	return ret;
}

/**
 * Adds a new metric to aggregator s.  The value from the metric is put
 * in the bucket matching the epoch contained in the metric.  In cases
 * where the contained epoch is too old or too new, the metric is
 * rejected.
 */
void
aggregator_putmetric(
		aggregator *s,
		const char *metric)
{
	char *v, *t;
	double val;
	long long int epoch;
	int slot;
	struct _bucket *bucket;

	/* get value */
	if ((v = strchr(metric, ' ')) == NULL || (t = strchr(v, ' ')) == NULL) {
		/* metric includes \n */
		fprintf(stderr, "aggregator: dropping incorrect metric: %s", metric);
		return;
	}

	val = atof(v + 1);
	epoch = atoll(t + 1);

	epoch -= s->buckets[0]->start;
	if (epoch < 0) {
		/* drop too old metric */
		s->dropped++;
		return;
	}

	slot = epoch / s->interval;
	if (slot >= s->bucketcnt) {
		fprintf(stderr, "aggregator: dropping metric too far in the future: %s",
				metric);
		s->dropped++;
		return;
	}

	pthread_mutex_lock(&s->bucketlock);
	s->received++;
	bucket = s->buckets[slot];
	if (bucket->cnt == 0) {
		bucket->cnt = 1;
		bucket->sum = val;
		bucket->max = val;
		bucket->min = val;
	} else {
		bucket->cnt++;
		bucket->sum += val;
		if (bucket->max < val)
			bucket->max = val;
		if (bucket->min > val)
			bucket->min = val;
	}
	pthread_mutex_unlock(&s->bucketlock);

	return;
}

/**
 * Checks if the oldest bucket should be expired, if so, sends out
 * computed aggregate metrics and moves the bucket to the end of the
 * list.
 */
static void *
aggregator_expire(void *unused)
{
	time_t now;
	aggregator *s;
	struct _bucket *b;
	struct _aggr_computes *c;
	int i;

	i = 0;
	while (keep_running) {
		sleep(1);
		now = time(NULL);

		for (s = aggregators; s != NULL; s = s->next) {
			while (s->buckets[0]->start + s->interval < now - s->expire) {
				/* yay, let's produce something cool */
				b = s->buckets[0];
				for (c = s->computes; c != NULL; c = c->next) {
					switch (c->type) {
						case SUM:
							fprintf(metricsock, "%s %f %lld\n",
									c->metric, b->sum,
									b->start + s->interval);
							break;
						case CNT:
							fprintf(metricsock, "%s %zd %lld\n",
									c->metric, b->cnt,
									b->start + s->interval);
							break;
						case MAX:
							fprintf(metricsock, "%s %f %lld\n",
									c->metric, b->max,
									b->start + s->interval);
							break;
						case MIN:
							fprintf(metricsock, "%s %f %lld\n",
									c->metric, b->min,
									b->start + s->interval);
							break;
						case AVG:
							fprintf(metricsock, "%s %f %lld\n",
									c->metric, b->sum / (double)b->cnt,
									b->start + s->interval);
							break;
					}
				}
				pthread_mutex_lock(&s->bucketlock);
				s->sent++;
				/* move the bucket to the end, to make room for new ones */
				memmove(&s->buckets[0], &s->buckets[1],
						sizeof(b) * (s->bucketcnt - 1));
				b->cnt = 0;
				b->start = s->buckets[s->bucketcnt - 2]->start + s->interval;
				s->buckets[s->bucketcnt - 1] = b;
				pthread_mutex_unlock(&s->bucketlock);
			}
		}
		/* push away whatever we produced */
		fflush(metricsock);
	}

	return NULL;
}

/**
 * Returns true if there are aggregators defined.
 */
char
aggregator_hasaggregators(void)
{
	return aggregators != NULL;
}

/**
 * Initialises and starts the aggregator.  Returns false when starting
 * failed, true otherwise.
 */
int
aggregator_start(void)
{
	int pipefds[2];

	/* create pipe to relay metrics over */
	if (pipe(pipefds) < 0) {
		fprintf(stderr, "aggregator: failed to create pipe\n");
		return 0;
	} else {
		if (dispatch_addconnection(pipefds[0]) != 0) {
			fprintf(stderr, "aggregator: unable a add connection\n");
			close(pipefds[0]);
			close(pipefds[1]);
			return 0;
		}
	}
	if ((metricsock = fdopen(pipefds[1], "w")) == NULL) {
		fprintf(stderr, "aggregator: failed to open pipe socket for writing\n");
		close(pipefds[0]);
		close(pipefds[1]);
		return 0;
	}
	if (pthread_create(&aggregatorid, NULL, aggregator_expire, NULL) != 0) {
		fprintf(stderr, "failed to start aggregator!\n");
		close(pipefds[0]);
		close(pipefds[1]);
		return 0;
	}

	return 1;
}

/**
 * Shuts down the aggregator.
 */
void
aggregator_stop(void)
{
	fclose(metricsock);
	pthread_join(aggregatorid, NULL);
}

/**
 * Returns an approximate number of received metrics by all aggregators.
 */
size_t
aggregator_get_received(void)
{
	size_t totreceived = 0;
	aggregator *a;

	for (a = aggregators; a != NULL; a = a->next)
		totreceived += a->received;

	return totreceived;
}

/**
 * Returns an approximate number of metrics sent by all aggregators.
 */
size_t
aggregator_get_sent(void)
{
	size_t totsent = 0;
	aggregator *a;

	for (a = aggregators; a != NULL; a = a->next)
		totsent += a->sent;

	return totsent;
}

/**
 * Returns an approximate number of dropped metrics by all aggregators.
 * Metrics are dropped if they are too much in the past (past expiry
 * time) or if they are too much in the future.
 */
size_t
aggregator_get_dropped(void)
{
	size_t totdropped = 0;
	aggregator *a;

	for (a = aggregators; a != NULL; a = a->next)
		totdropped += a->dropped;

	return totdropped;
}
