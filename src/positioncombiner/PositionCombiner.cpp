//******************************************************************************
//* File:   PositionCombiner.cpp
//* Author: Jon Newman <jpnewman snail mit dot edu>
//*
//* Copyright (c) Jon Newman (jpnewman snail mit dot edu)
//* All right reserved.
//* This file is part of the Oat project.
//* This is free software: you can redistribute it and/or modify
//* it under the terms of the GNU General Public License as published by
//* the Free Software Foundation, either version 3 of the License, or
//* (at your option) any later version.
//* This software is distributed in the hope that it will be useful,
//* but WITHOUT ANY WARRANTY; without even the implied warranty of
//* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//* GNU General Public License for more details.
//* You should have received a copy of the GNU General Public License
//* along with this source code.  If not, see <http://www.gnu.org/licenses/>.
//******************************************************************************

#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <thread>
#include <future>

#include "../../lib/shmemdf/Source.h"
#include "../../lib/shmemdf/Sink.h"
#include "../../lib/datatypes/Position2D.h"
#include "../../lib/utility/IOFormat.h"
#include "../../lib/utility/make_unique.h"

#include "PositionCombiner.h"

namespace oat {

PositionCombiner::PositionCombiner(
                        const std::vector<std::string> &position_source_addresses,
                        const std::string &position_sink_address) :
  name_("posicom[" + position_source_addresses[0] + "...->" + position_sink_address + "]")
, position_sink_address_(position_sink_address)
{

    for (auto &addr : position_source_addresses) {

        oat::Position2D pos(addr);
        positions_.push_back(std::move(pos));
        position_sources_.push_back(
            oat::NamedSource<oat::Position2D>(
                addr,
                std::make_unique<oat::Source< oat::Position2D>>()
            )
        );
    }
}

void PositionCombiner::connectToNodes() {

    // Establish our slot in each node
    for (auto &ps : position_sources_)
        ps.source->touch(ps.name);

    // Examine sample period of sources to make sure they are the same
    double sample_rate_hz;
    std::vector<double> all_ts;

    // Wait for sychronous start with sink when it binds the node
    for (auto &ps : position_sources_) {
        ps.source->connect();
        all_ts.push_back(ps.source->retrieve()->sample().period_sec().count());
    }

    if (!oat::checkSamplePeriods(all_ts, sample_rate_hz)) {
        std::cerr << oat::Warn(
                     "Warning: sample rates of sources are inconsistent.\n"
                     "This component forces synchronization at the lowest source sample rate.\n"
                     "You should probably use separate recorders to capture these sources.\n"
                     "specified sample rate set to: " + std::to_string(sample_rate_hz) + "\n"
                     );
    }

    // Bind to sink node and create a shared position
    position_sink_.bind(position_sink_address_, position_sink_address_);
    shared_position_ = position_sink_.retrieve();
}

bool PositionCombiner::process() {

    for (pvec_size_t i = 0; i !=  position_sources_.size(); i++) {

        // START CRITICAL SECTION //
        ////////////////////////////
        if (position_sources_[i].source->wait() == oat::NodeState::END)
            return true;

        positions_[i] = position_sources_[i].source->clone();

        position_sources_[i].source->post();
        ////////////////////////////
        //  END CRITICAL SECTION  //
    }

    combine(positions_, internal_position_);

    // START CRITICAL SECTION //
    ////////////////////////////

    // Wait for sources to read
    position_sink_.wait();

    *shared_position_ = internal_position_;

    // Tell sources there is new data
    position_sink_.post();

    ////////////////////////////
    //  END CRITICAL SECTION  //

    // Sink was not at END state
    return false;
}

} /* namespace oat */
