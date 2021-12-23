/*
 *
 * (C) 2013-21 - ntop.org
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "ntop_includes.h"

// #define THREAD_DEBUG

/* **************************************************** */

static void* startActivity(void* ptr)  {
  Utils::setThreadName(((ThreadedActivity*)ptr)->activityPath());

  ((ThreadedActivity*)ptr)->activityBody();
  return(NULL);
}

/* ******************************************* */

ThreadedActivity::ThreadedActivity(const char* _path,
				   u_int32_t _periodicity_seconds,
				   u_int32_t _max_duration_seconds,
				   bool _align_to_localtime,
				   bool _exclude_viewed_interfaces,
				   bool _exclude_pcap_dump_interfaces,
				   ThreadPool *_pool) {
  terminating = false;
  thread_started = false;
  interfaceTasksRunning = (ThreadedActivityState*) calloc(MAX_NUM_INTERFACE_IDS + 1 /* For the system interface */, sizeof(ThreadedActivityState));
  for(int i = 0; i < MAX_NUM_INTERFACE_IDS + 1; i++) {
    interfaceTasksRunning[i] = threaded_activity_state_sleeping;
  }
  threaded_activity_stats = new (std::nothrow) ThreadedActivityStats*[MAX_NUM_INTERFACE_IDS + 1 /* For the system interface */]();
  periodic_script = new (std::nothrow) PeriodicScript(_path,
                                                      _periodicity_seconds,
                                                      _max_duration_seconds,
                                                      _align_to_localtime,
                                                      _exclude_viewed_interfaces,
                                                      _exclude_pcap_dump_interfaces,
                                                      _pool);
  
  setDeadlineApproachingSecs();
  
#ifdef THREADED_DEBUG
  ntop->getTrace()->traceEvent(TRACE_WARNING, "[%p] Creating ThreadedActivity '%s'", this, path);
#endif
}

/* ******************************************* */

ThreadedActivity::~ThreadedActivity() {
  /* NOTE: terminateEnqueueLoop should have already been called by the PeriodicActivities
   * destructor. */
  terminateEnqueueLoop();

  if(threaded_activity_stats) {
    for(u_int i = 0; i < MAX_NUM_INTERFACE_IDS; i++) {
      if(threaded_activity_stats[i])
	delete threaded_activity_stats[i];
    }

    delete[] threaded_activity_stats;
  }

  if(interfaceTasksRunning)
    free(interfaceTasksRunning);

  if(periodic_script) delete periodic_script;
}

/* ******************************************* */

void ThreadedActivity::setDeadlineApproachingSecs() {
  if(getPeriodicity() <= 1)
    deadline_approaching_secs =  0;
  else if(getPeriodicity() <= 5)
    deadline_approaching_secs =  1;
  else if(getPeriodicity() <= 60)
    deadline_approaching_secs =  5;
  else /* > 60 secs */
    deadline_approaching_secs = 10;
}

/* ******************************************* */

/* Stop the possibly running pthreadLoop, so that new activities
 * won't be enqueued. */
void ThreadedActivity::terminateEnqueueLoop() {
  void *res;

  shutdown();

  if(thread_started) {
    pthread_join(pthreadLoop, &res);

#ifdef THREAD_DEBUG
    ntop->getTrace()->traceEvent(TRACE_NORMAL, "Joined thread %s", activityPath());
#endif

    thread_started = false;
  }
}

/* ******************************************* */

bool ThreadedActivity::isTerminating() {
  return(terminating
	 || ntop->getGlobals()->isShutdownRequested()
	 || ntop->getGlobals()->isShutdown());
};

/* ******************************************* */

ThreadedActivityState *ThreadedActivity::getThreadedActivityState(NetworkInterface *iface) const {
  if(iface) {
    /* As the system interface has id -1, we add 1 to the offset to access the array.
       The array is allocated in the constructor with MAX_NUM_INTERFACE_IDS + 1 to also
       accomodate the system interface */
    int stats_idx = iface->get_id() + 1;

    if(stats_idx >= 0 && stats_idx < MAX_NUM_INTERFACE_IDS + 1)
      return &interfaceTasksRunning[stats_idx];
    else {
      return NULL;
    }
  } else
    ntop->getTrace()->traceEvent(TRACE_ERROR, "Internal error. NULL interface.");

  return NULL;
}

/* ******************************************* */

const char* ThreadedActivity::get_state_label(ThreadedActivityState ta_state) {
  switch(ta_state) {
  case threaded_activity_state_sleeping:
    return("sleeping");
    break;
  case threaded_activity_state_queued:
    return("queued");
    break;
  case threaded_activity_state_running:
    return("running");
    break;
  case threaded_activity_state_unknown:
  default:
    return("unknown");
    break;
  }
}

/* ******************************************* */

static bool skipExecution(const char *path) {
#if 0
  if((ntop->getPrefs()->getTimeseriesDriver() != ts_driver_influxdb) &&
     (strcmp(path, TIMESERIES_SCRIPT_PATH) == 0))
    return(true);
#endif

  // Always execute periodic activities, thread timeseries.lua
  // is now also used by rrds to dequeue writes
  return(false);
}

/* ******************************************* */

void ThreadedActivity::set_state(NetworkInterface *iface, ThreadedActivityState ta_state) {
  ThreadedActivityState *cur_state = getThreadedActivityState(iface);

  if(cur_state) {
    if((*cur_state == threaded_activity_state_queued
	&& ta_state != threaded_activity_state_running)
       || (*cur_state == threaded_activity_state_running
	   && ta_state != threaded_activity_state_sleeping))
      ntop->getTrace()->traceEvent(TRACE_ERROR, "Internal error. Invalid state transition. [path: %s][iface: %s]",
				   activityPath(), iface->get_name());
    /* Everything is OK, let's set the state. */
    *cur_state = ta_state;
  }
}

/* ******************************************* */

ThreadedActivityState ThreadedActivity::get_state(NetworkInterface *iface) const {
  ThreadedActivityState *cur_state = getThreadedActivityState(iface);

  if(cur_state)
    return *cur_state;

  return threaded_activity_state_unknown;

}

/* ******************************************* */

void ThreadedActivity::set_state_sleeping(NetworkInterface *iface) {
  set_state(iface, threaded_activity_state_sleeping);
}

/* ******************************************* */

void ThreadedActivity::set_state_queued(NetworkInterface *iface) {
  ThreadedActivityStats *ta_stats = getThreadedActivityStats(iface, true /* Allocate if missing */);

  set_state(iface, threaded_activity_state_queued);

  if(ta_stats)
    ta_stats->updateStatsQueuedTime(time(NULL));
}

/* ******************************************* */

void ThreadedActivity::set_state_running(NetworkInterface *iface) {
  set_state(iface, threaded_activity_state_running);
}

/* ******************************************* */

bool ThreadedActivity::isQueueable(NetworkInterface *iface) const {
  ThreadedActivityState *cur_state = getThreadedActivityState(iface);

  if(cur_state && *cur_state == threaded_activity_state_sleeping)
    return true;

  return false;
}

/* ******************************************* */

bool ThreadedActivity::isDeadlineApproaching(time_t deadline) const {
  if (terminating)
    return true;

  /*
    The deadline is approaching if the current time is closer than deadline_approaching_secs
    with reference to the deadline passed as parameter
  */
  bool res = deadline - time(NULL) <= deadline_approaching_secs;

  return res;
}

/* ******************************************* */

/* NOTE: this runs into a separate thread, launched by PeriodicActivities
 * after creation. */
void ThreadedActivity::activityBody() {
  if(getPeriodicity() == 0)       /* The script is not periodic */
    aperiodicActivityBody();
  else
    periodicActivityBody();
}

/* ******************************************* */

void ThreadedActivity::run() {
  bool pcap_dump_only = true;

  for(int i = 0; i < ntop->get_num_interfaces(); i++) {
    NetworkInterface *iface = ntop->getInterface(i);
    if(iface && iface->getIfType() != interface_type_PCAP_DUMP)
      pcap_dump_only = false;
  }
  /* Don't schedule periodic activities it we are processing pcap files only. */
  if (excludePcap() && pcap_dump_only)
    return;

  if(pthread_create(&pthreadLoop, NULL, startActivity, (void*)this) == 0) {
    thread_started = true;
#ifdef __linux__
    Utils::setThreadAffinityWithMask(pthreadLoop, ntop->getPrefs()->get_other_cpu_affinity_mask());
#endif
  }
}

/* ******************************************* */

ThreadedActivityStats *ThreadedActivity::getThreadedActivityStats(NetworkInterface *iface, bool allocate_if_missing) {
  ThreadedActivityStats *ta = NULL;

  if(!isTerminating() && iface) {
    /* As the system interface has id -1, we add 1 to the offset to access the array of stats.
       The array of stats is allocated in the constructor with MAX_NUM_INTERFACE_IDS + 1 to also
       accomodate the system interface */
    int stats_idx = iface->get_id() + 1;

    if(stats_idx >= 0 && stats_idx < MAX_NUM_INTERFACE_IDS + 1) {
      if(!threaded_activity_stats[stats_idx]) {
	if(allocate_if_missing) {
	  try {
	    ta = new ThreadedActivityStats(this);
	  } catch(std::bad_alloc& ba) {
	    return NULL;
	  }
	  threaded_activity_stats[stats_idx] = ta;
	}
      } else
	ta = threaded_activity_stats[stats_idx];
    }
  }

  return ta;
}

/* ******************************************* */

void ThreadedActivity::updateThreadedActivityStatsBegin(NetworkInterface *iface, struct timeval *begin) {
  ThreadedActivityStats *ta = getThreadedActivityStats(iface, true /* Allocate if missing */);

  if(ta)
    ta->updateStatsBegin(begin);
}

/* ******************************************* */

void ThreadedActivity::updateThreadedActivityStatsEnd(NetworkInterface *iface, u_long latest_duration) {
  ThreadedActivityStats *ta = getThreadedActivityStats(iface, true /* Allocate if missing */);

  if(ta)
    ta->updateStatsEnd(latest_duration);
}

/* ******************************************* */

/* Run a one-shot script / accurate (e.g. second) periodic script */
void ThreadedActivity::runSystemScript(time_t now) {
  struct stat buf;
  char script_path[MAX_PATH];
  
  snprintf(script_path, sizeof(script_path), "%s/system/%s",
	   ntop->get_callbacks_dir(), activityPath());

  if(stat(script_path, &buf) == 0) {
    set_state_running(ntop->getSystemInterface());
    runScript(now, script_path, ntop->getSystemInterface(), now + getMaxDuration() /* this is the deadline */);
    set_state_sleeping(ntop->getSystemInterface());
  } else
    ntop->getTrace()->traceEvent(TRACE_WARNING, "Unable to find script %s", activityPath());
}

/* ******************************************* */

/* Run a script - both periodic and one-shot scripts are called here */
void ThreadedActivity::runScript(time_t now, char *script_path, NetworkInterface *iface, time_t deadline) {
  LuaEngine *l = NULL;
  u_long msec_diff;
  struct timeval begin, end;
  ThreadedActivityStats *thstats = getThreadedActivityStats(iface, true);

  if(!iface)
    return;

  if(strcmp((activityPath()), SHUTDOWN_SCRIPT_PATH) && isTerminating())
    return;

  if(iface->isViewed() && excludeViewedIfaces())
    return;

#ifdef THREADED_DEBUG
  ntop->getTrace()->traceEvent(TRACE_WARNING, "[%p] Running %s", this, activityPath());
#endif

  ntop->getTrace()->traceEvent(TRACE_INFO, "Running %s (iface=%p)", script_path, iface);

  l = loadVm(script_path, iface, now);
  if(!l) {
    ntop->getTrace()->traceEvent(TRACE_ERROR, "Unable to load the Lua vm [%s][vm: %s]", iface->get_name(), activityPath());
    return;
  }

  /* Set the deadline and the threaded activity in the vm so they can be accessed */
  l->setThreadedActivityData(this, thstats, deadline);

  if(thstats) {
    thstats->setDeadline(deadline);
    thstats->setCurrentProgress(0);

    /* Reset the internal state for the current execution */
    thstats->setNotExecutedAttivity(false);
    thstats->setSlowPeriodicActivity(false);
  }

  gettimeofday(&begin, NULL);
  updateThreadedActivityStatsBegin(iface, &begin);

  /* Set the current time globally  */
  lua_pushinteger(l->getState(), now);
  lua_setglobal(l->getState(), "_now");
  l->run_loaded_script();

  gettimeofday(&end, NULL);
  msec_diff = (end.tv_sec - begin.tv_sec) * 1000 + (end.tv_usec - begin.tv_usec) / 1000;
  updateThreadedActivityStatsEnd(iface, msec_diff);

  if(thstats && isDeadlineApproaching(deadline))
    thstats->setSlowPeriodicActivity(true);

  if(l)
    delete l;
}

/* ******************************************* */

LuaEngine* ThreadedActivity::loadVm(char *script_path, NetworkInterface *iface, time_t when) {
  LuaEngine *l = NULL;

  try {
    /* NOTE: this needs to be deallocated by the caller */
    l = new LuaEngine(NULL);

    if(l->load_script(script_path, iface) != 0) {
      delete l;
      l = NULL;
    }
  } catch(std::bad_alloc& ba) {
    l = NULL;
  }

  return(l);
}

/* ******************************************* */

void ThreadedActivity::aperiodicActivityBody() {
  if(!isTerminating())
    runSystemScript(time(NULL));
}

/* ******************************************* */

void ThreadedActivity::periodicActivityBody() {
  u_int now;
  u_int32_t next_deadline, next_schedule = (u_int32_t)time(NULL);

  next_schedule = Utils::roundTime(next_schedule, getPeriodicity(), 
				   alignToLocalTime() ? ntop->get_time_offset() : 0);

  while(!isTerminating()) {
    now = (u_int)time(NULL);

    if(now >= next_schedule) {
      next_deadline = now + getMaxDuration(); /* deadline is max_duration_secs from now */
      next_schedule = Utils::roundTime(now, getPeriodicity(), 
				       alignToLocalTime() ? ntop->get_time_offset() : 0);

      if(!skipExecution(activityPath()))
	schedulePeriodicActivity(getPool(), now, next_deadline);
    }

    sleep(1);
  }

  /* ntop->getTrace()->traceEvent(TRACE_NORMAL, "Terminating %s(%s) exit", __FUNCTION__, path); */
}

/* ******************************************* */

/* This function enqueues the periodic activity job into the ThreadPool.
 * The ThreadPool, running into another thread, will dequeue the job and call
 * ThreadedActivity::runScript. The variables interfaceTasksRunning
 * are used to ensure that only a single instance of the job is running for a given
 * NetworkInterface. */
void ThreadedActivity::schedulePeriodicActivity(ThreadPool *pool, time_t scheduled_time, time_t deadline) {
  /* Schedule per system / interface */
  char dir_path[MAX_PATH];
  struct stat buf;
  DIR *dir_struct;
  struct dirent *ent;

#ifdef THREAD_DEBUG
  ntop->getTrace()->traceEvent(TRACE_NORMAL, "%s(%u)", __FUNCTION__, scheduled_time);
#endif
  
  /* Schedule system script */
  snprintf(dir_path, sizeof(dir_path), "%s/%s/system/",
	   ntop->get_callbacks_dir(), activityPath());

#ifdef NTOPNG_PRO
  if(stat(dir_path, &buf)) {
    /* Attempt to locate and execute the callback under the pro callbacks */
    snprintf(dir_path, sizeof(dir_path), "%s/%s/system/",
	     ntop->get_pro_callbacks_dir(), activityPath());
  }
#endif

  if(stat(dir_path, &buf) == 0) {
    /* Open the directory and run all the scripts inside it */
    if ((dir_struct = opendir (dir_path)) != NULL) {
      while ((ent = readdir (dir_struct)) != NULL) {
        if (ent->d_name[0] != '.') { 
          char script_path[MAX_PATH];
	  /* Schedule interface script, one for each interface */
          snprintf(script_path, sizeof(script_path), "%s%s", dir_path, ent->d_name);

          if(pool->queueJob(this, script_path, ntop->getSystemInterface(), scheduled_time, deadline)) {
#ifdef THREAD_DEBUG
	    ntop->getTrace()->traceEvent(TRACE_NORMAL, "Queued system job %s", script_path);
#endif
          }
        }
      }

      closedir (dir_struct);
    }
  }

  /* Schedule interface script, one for each interface */
  snprintf(dir_path, sizeof(dir_path), "%s/%s/interface/",
	   ntop->get_callbacks_dir(), activityPath());

#ifdef NTOPNG_PRO
  if(stat(dir_path, &buf)) {
    /* Attempt at locating and executing the callback under the pro callbacks */
    snprintf(dir_path, sizeof(dir_path), "%s/%s/interface/",
	     ntop->get_pro_callbacks_dir(), activityPath());
  }
#endif

  if (stat(dir_path, &buf) == 0) {
    /* Open the directory e run all the scripts inside it */
    if ((dir_struct = opendir (dir_path)) != NULL) {
      while ((ent = readdir (dir_struct)) != NULL) {
        if (ent->d_name[0] != '.') { /* Excluding . and .. directories */
          for(int i = 0; i < ntop->get_num_interfaces(); i++) {
            NetworkInterface *iface = ntop->getInterface(i);
            
            /* Running the script for each interface if it's not a PCAP */
            if (iface &&
		(iface->getIfType() != interface_type_PCAP_DUMP || !excludePcap())) {      
              char script_path[MAX_PATH];
	      /* Schedule interface script, one for each interface */
              snprintf(script_path, sizeof(script_path), "%s%s", dir_path, ent->d_name);       
         
              if(pool->queueJob(this, script_path, iface, scheduled_time, deadline)) {
#ifdef THREAD_DEBUG
                ntop->getTrace()->traceEvent(TRACE_NORMAL, "Queued interface job %s [%s]", script_path, iface->get_name());
#endif
              }
            }
          }
        }
      }
      
      closedir (dir_struct);
    }
  }
}

/* ******************************************* */

void ThreadedActivity::lua(NetworkInterface *iface, lua_State *vm) {
  ThreadedActivityStats *ta = getThreadedActivityStats(iface, false /* Do not allocate if missing */);

  if(ta) {
    lua_newtable(vm);

    ta->lua(vm);

    lua_push_str_table_entry(vm, "state", get_state_label(get_state(iface)));
    lua_push_uint64_table_entry(vm, "periodicity", getPeriodicity());
    lua_push_uint64_table_entry(vm, "max_duration_secs", getMaxDuration());
    lua_push_uint64_table_entry(vm, "deadline_secs", deadline_approaching_secs);

    lua_pushstring(vm, activityPath() ? activityPath() : "");
    lua_insert(vm, -2);
    lua_settable(vm, -3);
  }
}

/* ******************************************* */

const char *ThreadedActivity::activityPath() {
  return (periodic_script ? periodic_script->getPath() : "");
}

/* ******************************************* */

u_int32_t ThreadedActivity::getPeriodicity() { 
  return (periodic_script ? periodic_script->getPeriodicity() : 0); 
}

/* ******************************************* */

u_int32_t ThreadedActivity::getMaxDuration() { 
  return (periodic_script ? periodic_script->getMaxDuration() : 0); 
}

/* ******************************************* */

bool ThreadedActivity::excludePcap() { 
  return (periodic_script ? periodic_script->excludePcap() : false);
}

/* ******************************************* */

bool ThreadedActivity::excludeViewedIfaces() { 
  return (periodic_script ? periodic_script->excludeViewedIfaces() : false);
}

/* ******************************************* */

bool ThreadedActivity::alignToLocalTime() { 
  return (periodic_script ? periodic_script->alignToLocalTime() : false);
}

/* ******************************************* */

ThreadPool *ThreadedActivity::getPool() { 
  return (periodic_script ? periodic_script->getPool() : NULL);
}


