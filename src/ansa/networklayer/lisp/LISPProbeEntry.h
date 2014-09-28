//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
// 
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
// 

#ifndef LISPPROBEENTRY_H_
#define LISPPROBEENTRY_H_

#include "LISPRLocator.h"
#include "LISPEidPrefix.h"
#include "LISPTStructs.h"

typedef std::pair<LISPEidPrefix, LISPRLocator*> EidStat;
typedef std::list<EidStat> EidsStats;
typedef EidsStats::const_iterator EidCItem;
typedef EidsStats::iterator EidItem;

class LISPProbeEntry {
  public:
    LISPProbeEntry(IPvXAddress nrloc);
    virtual ~LISPProbeEntry();

    bool operator== (const LISPProbeEntry& other) const;

    const IPvXAddress& getRlocAddr() const;
    const simtime_t& getLastTimeProbed() const;
    void setLastTimeProbed(const simtime_t lastTimeProbed);
    const EidsStats& getEids() const;

    std::string info() const;

    bool hasEid(const LISPEidPrefix& eid);
    void addEid(const LISPEidPrefix eid, LISPRLocator* rloc);
    LISPEidPrefix getNextEid();

    void setRlocStatusForEid(const LISPEidPrefix& eid, LISPRLocator::LocatorState stat);
    void setRlocStatusForAllEids(LISPRLocator::LocatorState stat);

  private:
    IPvXAddress rlocAddr;
    EidsStats eids;
    simtime_t lastTimeProbed;
    bool curInit;
    EidItem curEid;
};

//Free function
std::ostream& operator<< (std::ostream& os, const LISPProbeEntry& entry);
std::ostream& operator<< (std::ostream& os, const EidStat& es);
std::ostream& operator<< (std::ostream& os, const EidsStats& eids);

#endif /* LISPPROBEENTRY_H_ */
