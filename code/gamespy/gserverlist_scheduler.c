#include "gserverlist_scheduler.h"

void GServerListSchedulerInit(GServerListScheduler *scheduler)
{
	scheduler->next       = 0;
	scheduler->completed  = 0;
	scheduler->responsive = 0;
	scheduler->timedout   = 0;
	scheduler->retryCount = 0;
	scheduler->nextRetry  = 0;
}

int GServerListSchedulerClaim(GServerListScheduler *scheduler, int discovered)
{
	if (scheduler->next >= discovered) {
		return -1;
	}
	return scheduler->next++;
}

void GServerListSchedulerComplete(GServerListScheduler *scheduler, int responsive)
{
	scheduler->completed++;
	if (responsive) {
		scheduler->responsive++;
	} else {
		scheduler->timedout++;
	}
}

void GServerListSchedulerQueueRetry(GServerListScheduler *scheduler)
{
	scheduler->retryCount++;
}

int GServerListSchedulerClaimRetry(GServerListScheduler *scheduler)
{
	if (scheduler->nextRetry >= scheduler->retryCount) {
		return -1;
	}
	return scheduler->nextRetry++;
}

int GServerListSchedulerDone(
	const GServerListScheduler *scheduler, int discovered, int active, int mastersFinished
)
{
	return mastersFinished && scheduler->next >= discovered && scheduler->nextRetry >= scheduler->retryCount
		&& active == 0;
}

int GServerListSchedulerBudget(int totalBudget, int activeLists)
{
	int budget;

	if (activeLists < 1) {
		activeLists = 1;
	}
	budget = totalBudget / activeLists;
	return budget > 0 ? budget : 1;
}
