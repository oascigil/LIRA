/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2014-2015,  Regents of the University of California,
 *                           Arizona Board of Regents,
 *                           Colorado State University,
 *                           University Pierre & Marie Curie, Sorbonne University,
 *                           Washington University in St. Louis,
 *                           Beijing Institute of Technology,
 *                           The University of Memphis.
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "multicast-strategy.hpp"

namespace nfd {
namespace fw {
NFD_LOG_INIT("MulticastStrategy");

const Name MulticastStrategy::STRATEGY_NAME("ndn:/localhost/nfd/strategy/multicast/%FD%01");
NFD_REGISTER_STRATEGY(MulticastStrategy);

MulticastStrategy::MulticastStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder, name)
{
}

/** \brief determines whether a NextHop is eligible
 *  \param currentDownstream incoming FaceId of current Interest
 *  \param wantUnused if true, NextHop must not have unexpired OutRecord
 *  \param now time::steady_clock::now(), ignored if !wantUnused
 */
static inline bool
predicate_NextHop_eligible(const shared_ptr<pit::Entry>& pitEntry,
  const fib::NextHop& nexthop, FaceId currentDownstream,
  bool wantUnused = false,
  time::steady_clock::TimePoint now = time::steady_clock::TimePoint::min())
{
  shared_ptr<Face> upstream = nexthop.getFace();

  // upstream is current downstream
  if (upstream->getId() == currentDownstream)
    return false;

  // forwarding would violate scope
  if (pitEntry->violatesScope(*upstream))
    return false;

  if (wantUnused) {
    // NextHop must not have unexpired OutRecord
    pit::OutRecordCollection::const_iterator outRecord = pitEntry->getOutRecord(*upstream);
    if (outRecord != pitEntry->getOutRecords().end() &&
        outRecord->getExpiry() > now) {
      return false;
    }
  }

  return true;
}

void
MulticastStrategy::afterReceiveInterest(const Face& inFace,
                   const Interest& interest,
                   shared_ptr<fib::Entry> fibEntry,
                   shared_ptr<fib::Entry> sitEntry,
                   shared_ptr<pit::Entry> pitEntry)
{
  NFD_LOG_DEBUG("afterReceiveInterest interest=" << interest.getName());
  int sdc = pitEntry->getFloodFlag();
  uint32_t cost = 1;
  if(fibEntry->hasNextHops())
  {
    cost = fibEntry->getNextHops()[0].getCost();
  }

  if(pitEntry->getDestinationFlag() && static_cast<bool>(sitEntry) /*&& sdc > 0*/)
  {//Destination Flag is set so follow SIT
    const fib::NextHopList& nexthops = sitEntry->getNextHops();
    fib::NextHopList::const_iterator it = nexthops.end();

    //Disable suppression of out-going interests
    RetxSuppression::Result suppression = m_retxSuppression.decide(inFace, interest, *pitEntry);
    if (suppression == RetxSuppression::NEW) {
      // forward to nexthop with lowest cost except downstream
      it = std::find_if(nexthops.begin(), nexthops.end(),
        bind(&predicate_NextHop_eligible, pitEntry, _1, inFace.getId(),
           true, time::steady_clock::TimePoint::min()));

      if (it == nexthops.end()) {
        NFD_LOG_DEBUG(interest << " from=" << inFace.getId() << " noNextHop");
        this->rejectPendingInterest(pitEntry);
        return;
      }

      shared_ptr<Face> outFace = it->getFace();
      //sdc--;
      if (pitEntry->canForwardTo(*outFace)) {

        (*pitEntry).setFloodFlag(sdc);
        this->sendInterest(pitEntry, outFace);
        NFD_LOG_DEBUG(interest << " from=" << inFace.getId()
                           << " newPitEntry-to=" << outFace->getId());
      }
    }
    if (suppression == RetxSuppression::SUPPRESS) {
      NFD_LOG_DEBUG(interest << " from=" << inFace.getId()
                           << " suppressed");
      return;
    }
  } 
  else if(!pitEntry->getDestinationFlag() && static_cast<bool>(sitEntry) && sdc > 0 && cost > 0) 
  {   //Destination Flag is not set
    const fib::NextHopList& nexthops = sitEntry->getNextHops();
    //iterate through the nexthops and forward the interest (except inFace)
    for (fib::NextHopList::const_iterator it = nexthops.begin(); it != nexthops.end(); ++it) 
    {
      if (!predicate_NextHop_eligible(pitEntry, *it, inFace.getId(), true))
        continue; //skip downstream
      //Disable suppression of out-going interests
      RetxSuppression::Result suppression = m_retxSuppression.decide(inFace, interest, *pitEntry);
      if (suppression == RetxSuppression::NEW) {
        shared_ptr<Face> outFace = it->getFace();
        sdc = sdc - 1;
	     (*pitEntry).setDestinationFlag(); 
        (*pitEntry).setFloodFlag(sdc);
        this->sendInterest(pitEntry, outFace);
	     (*pitEntry).clearDestinationFlag(); 
        if(0 == sdc)
          break;
      } 
    }  
  }
  if(!pitEntry->getDestinationFlag() && sdc > 0) 
  {
    const fib::NextHopList& nexthops = fibEntry->getNextHops();
    fib::NextHopList::const_iterator it = nexthops.end();
    //Disable suppression of out-going interests 
    RetxSuppression::Result suppression =
        m_retxSuppression.decide(inFace, interest, *pitEntry);
    if (suppression == RetxSuppression::NEW) {
    // forward to nexthop with lowest cost except downstream
      it = std::find_if(nexthops.begin(), nexthops.end(),
           bind(&predicate_NextHop_eligible, pitEntry, _1, inFace.getId(),
             true, time::steady_clock::TimePoint::min()));

      if (it == nexthops.end()) {
          NFD_LOG_DEBUG(interest << " from=" << inFace.getId() << " noNextHop");
          this->rejectPendingInterest(pitEntry);
          return;
      }

      shared_ptr<Face> outFace = it->getFace();
      sdc--;
      (*pitEntry).setFloodFlag(sdc);
      this->sendInterest(pitEntry, outFace);
      NFD_LOG_DEBUG(interest << " from=" << inFace.getId()
                         << " newPitEntry-to=" << outFace->getId());
      //return;
    }
  } //if sdc > 0
  if (!pitEntry->hasUnexpiredOutRecords()) {
    this->rejectPendingInterest(pitEntry);
  }
}

} // namespace fw
} // namespace nfd
