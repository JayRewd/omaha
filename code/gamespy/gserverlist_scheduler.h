#ifndef _GSERVERLIST_SCHEDULER_H
#define _GSERVERLIST_SCHEDULER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GServerListScheduler {
	int next;
	int completed;
	int responsive;
	int timedout;
	int retryCount;
	int nextRetry;
} GServerListScheduler;

void GServerListSchedulerInit(GServerListScheduler *scheduler);
int  GServerListSchedulerClaim(GServerListScheduler *scheduler, int discovered);
void GServerListSchedulerComplete(GServerListScheduler *scheduler, int responsive);
void GServerListSchedulerQueueRetry(GServerListScheduler *scheduler);
int  GServerListSchedulerClaimRetry(GServerListScheduler *scheduler);
int  GServerListSchedulerDone(
	const GServerListScheduler *scheduler, int discovered, int active, int mastersFinished
);
int GServerListSchedulerBudget(int totalBudget, int activeLists);

#ifdef __cplusplus
}
#endif

#endif
