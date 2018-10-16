/*
 *
 * (C) 2015-18 - ntop.org
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

#ifndef _TIMELINE_EXTRACT_H_
#define _TIMELINE_EXTRACT_H_

#include "ntop_includes.h"

class TimelineExtract {
 private:
  pthread_t extraction_thread;
  bool running;
  bool shutdown;

  struct {
    NetworkInterface *iface;
    u_int32_t id;
    time_t from;
    time_t to;
    char *bpf_filter;
  } extraction;

 public:
  TimelineExtract();
  ~TimelineExtract();
  inline NetworkInterface *getNetworkInterface() { return extraction.iface; };
  inline u_int32_t getID() { return extraction.id; };
  inline time_t getFrom() { return extraction.from; };
  inline time_t getTo() { return extraction.to; };
  inline const char *getFilter() { return extraction.bpf_filter; };
  inline bool isRunning() { return running; };
  void stop();
  bool extract(NetworkInterface *iface, time_t from, time_t to, const char *bpf_filter);
  void runExtractionJob(u_int32_t id, NetworkInterface *iface, time_t from, time_t to, const char *bpf_filter);
  void cleanupJob();
};

#endif /* _TIMELINE_EXTRACT_H_ */
