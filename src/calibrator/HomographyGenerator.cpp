//******************************************************************************
//* File:   HomographyGenerator.cpp
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
//****************************************************************************

#include "OatConfig.h" // Generated by CMake

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <string>
#include <iostream>
#include <fstream>

#include <boost/io/ios_state.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>

#include <cpptoml.h>
#include "../../lib/utility/OatTOMLSanitize.h"
#include "../../lib/utility/IOUtility.h"
#include "../../lib/utility/IOFormat.h"

#include "Saver.h"
#include "UsagePrinter.h"
#include "PathChanger.h"
#include "HomographyGenerator.h"

namespace oat {

HomographyGenerator::HomographyGenerator(const std::string& frame_source_name, const EstimationMethod method) :
  Calibrator(frame_source_name)
, homography_valid_(false)
, method_(method) {

    // if (interactive_) { // TODO: Generalize to accept points from a file without interactive session

#ifdef OAT_USE_OPENGL
        try {
            cv::namedWindow(name(), cv::WINDOW_OPENGL & cv::WINDOW_KEEPRATIO);
        } catch (cv::Exception& ex) {
            oat::whoWarn(name(), "OpenCV not compiled with OpenGL support."
                    " Falling back to OpenCV's display driver.\n");
            cv::namedWindow(name(), cv::WINDOW_NORMAL & cv::WINDOW_KEEPRATIO);
        }
#else
        cv::namedWindow(name(), cv::WINDOW_NORMAL & cv::WINDOW_KEEPRATIO);
#endif

        //set the callback function for any mouse event
        cv::setMouseCallback(name(), onMouseEvent, this);

        std::cout << "Starting interactive session.\n";
        UsagePrinter usage;
        accept(&usage, std::cout);
   // }
}

void HomographyGenerator::configure(const std::string& config_file, const std::string& config_key) {

    // TODO: Provide list of pixel points and world coords directly from file.

    // Available options
    std::vector<std::string> options {""};

    // This will throw cpptoml::parse_exception if a file
    // with invalid TOML is provided
    auto config = cpptoml::parse_file(config_file);

    // See if a camera configuration was provided
    if (config->contains(config_key)) {

        // Get this components configuration table
        auto this_config = config->get_table(config_key);

        // Check for unknown options in the table and throw if you find them
        oat::config::checkKeys(options, this_config);

    } else {
        throw (std::runtime_error(oat::configNoTableError(config_key, config_file)));
    }
}

void HomographyGenerator::calibrate(cv::Mat& frame) {

    if (clicked_) {
        frame = drawMousePoint(frame);
    }

    cv::imshow(name(), frame);
    char command = cv::waitKey(1);

    switch (command) {

        case 'a': // Add point to data map
        {
            addDataPoint();
            break;
        }

        case 'd': // Delete point from data map
        {
            removeDataPoint();
            break;
        }
        case 'f': // Change the calibration save path
        {
            PathChanger changer;
            accept(&changer);
            break;
        }
        case 'g': // Generate homography
        {
            generateHomography();
            break;
        }
        case 'h': // Display help dialog
        {
            UsagePrinter usage;
            accept(&usage, std::cout);
            break;
        }
        case 'm': // Select homography estimation method
        {
            selectHomographyMethod();
            break;
        }
        case 'p': // Print current data set
        {
            printDataPoints(std::cout);
            break;
        }
        case 's': // Persist homography to file
        {
            Saver saver("homography", calibration_save_path_);
            accept(&saver);
            break;
        }
    }
}

void HomographyGenerator::accept(CalibratorVisitor* visitor) {

    visitor->visit(this);
}

void HomographyGenerator::accept(OutputVisitor* visitor, std::ostream& out) {

    visitor->visit(this, out);
}
    
int HomographyGenerator::addDataPoint() {

    try {

        // Make sure the user has actually selected a point on the image
        if (!clicked_) {
            std::cerr << oat::Error("Click a point on the image to add it to the data set.\n");
            UsagePrinter usage;
            accept(&usage, std::cout);
            return -1;
        }

        // Check if mouse_pt_ is already in the pixels_ vector
        if (std::find(pixels_.begin(), pixels_.end(),
                    static_cast<cv::Point2f>(mouse_pt_)) != pixels_.end()) {

            std::cerr << oat::Error("This coordinate already exists in the source data set.\n")
                      << oat::Error("Select another point or delete before re-adding it.\n");
            return -1;
        }

        // Instruct
        std::cout << "Enter <x y> world coordinate followed by <enter>: ";

        // Receive 2 doubles, throw if not
        double p;
        constexpr int num_coords = 2;
        double dst_pt[2];

        for (int i = 0; i < num_coords; i++) {
            if (std::cin >> p)
                dst_pt[i] = p;
            else
                throw std::invalid_argument("World coordinates must be a pair of numerical values.");
        }

        // Check if dst_pt is already in the world_points_ vector
        if (std::find(world_points_.begin(), world_points_.end(),
                    cv::Point2f(dst_pt[0], dst_pt[1])) != world_points_.end()) {

            std::cerr << oat::Error("This coordinate already exists in the destination data set.\n")
                      << oat::Error("World coordinates must be unique for the homography to be well-defined.\n");
            return -1;
        }

        pixels_.push_back(cv::Point2f(mouse_pt_.x, mouse_pt_.y));
        world_points_.push_back(cv::Point2f(dst_pt[0], dst_pt[1]));

        std::cout << "Coordinate added to map.\n";

    } catch (std::invalid_argument ex) {

        oat::ignoreLine(std::cin);
        std::cerr << oat::Error("Invalid argument: ") << oat::Error(ex.what()) << "\n";
        return -1;
    }

    return 0;
}

int HomographyGenerator::removeDataPoint() {

    try {

        point_size_t idx;
        std::cout << "Enter data index to delete. Enter 'q' to do nothing: ";
        if (!(std::cin >> idx)) {

            oat::ignoreLine(std::cin);
            std::cout << "Delete mode terminated.\n";
            return -1;
        }

        if (idx < 0 || idx >= pixels_.size()) {
            std::cerr << oat::Error("Index out of bounds. Delete unsuccessful.\n");
            printDataPoints(std::cout);
            return -1;
        }

        pixels_.erase(pixels_.begin() + idx);
        world_points_.erase(world_points_.begin() + idx);

        std::cout << "Data point at index " << idx << " was deleted.\n";
    } catch (std::invalid_argument ex) {

        // Flush cin and report error
        oat::ignoreLine(std::cin);
        std::cerr << oat::Error("Invalid argument:") << oat::Error(ex.what()) << "\n";
        return -1;
    }

    return 0;
}

int HomographyGenerator::selectHomographyMethod() {

    std::cout << "Available homgraphy estimation methods:\n"
              << "[0] Robust\n"
              << "[1] Regular\n"
              << "[2] Exact\n"
              << "Enter a numerical selection: ";

    int sel {-1};
    if (!(std::cin >> sel)) {
        oat::ignoreLine(std::cin);
        return -1;
    }

    switch (sel) {
        case 0:
        {
            method_ = EstimationMethod::ROBUST;
            std::cout << "Estimation method set to robust.\n";
            break;
        }
        case 1:
        {
            method_ = EstimationMethod::REGULAR;
            std::cout << "Estimation method set to regular.\n";
            break;
        }
        case 2:
        {
            method_ = EstimationMethod::EXACT;
            std::cout << "Estimation method set to exact.\n";
            break;
        }
        default:
        {
            std::cerr << oat::Error("Invalid selection.\n");
            return -1;
        }

        return 0;
    }
}

void HomographyGenerator::printDataPoints(std::ostream& out) {

    // Save stream state. When ifs is destructed, the stream will
    // return to default format.
    boost::io::ios_flags_saver ifs(out);

    // Table format parameters
    constexpr int entry_width {25};
    constexpr int prec {5};

    out << "Current homography data set:\n"
        << std::left
        << "Index  "
        << std::setw(entry_width)
        << "Pixels"
        << "World\n";

    for(point_size_t i = 0; i != pixels_.size(); i++) {

        // Format table entries
        std::ostringstream ix_out, px_out, wd_out;

        ix_out << i
               << ":  ";

        px_out << "["
               << std::setprecision(prec) << pixels_[i].x
               << ", "
               << std::setprecision(prec) << pixels_[i].y
               << "]";

        wd_out << "["
               << std::setprecision(prec) << world_points_[i].x
               << ", "
               << std::setprecision(prec) << world_points_[i].y
               << "]";

        // Single table row
        out << std::right
            << std::setw(7) << ix_out.str()
            << std::left
            << std::setw(entry_width) << px_out.str()
            << std::setw(entry_width) << wd_out.str()
            << '\n';
    }

    out << "\n";
}

int HomographyGenerator::generateHomography() {

    // Check if there are enough points to try
    if (pixels_.size() < 4) {
        std::cerr << oat::Error("At least 4 data points are required to compute a homography. \n");
        printDataPoints(std::cout);

        return -1;
    }

    switch (method_) {
        case EstimationMethod::ROBUST :
        {
            homography_ = cv::findHomography(pixels_, world_points_, cv::RANSAC);
            break;
        }
        case EstimationMethod::REGULAR :
        {
            homography_ = cv::findHomography(pixels_, world_points_, 0);
            break;
        }
        case EstimationMethod::EXACT :
        {
            if (pixels_.size() != 4) {
                std::cerr << oat::Error("Exactly 4 points are used to calculate an exact homography.\n")
                          << oat::Error("Ensure there are exactly 4 points in your data set by adding or deleting.\n");
                printDataPoints(std::cout);
                return -1;
            } else {
                homography_ = cv::getPerspectiveTransform(pixels_, world_points_);
            }
            break;
        }
    }

    if (!homography_.empty()) {
        homography_valid_ = true;
        std::cout << "Homography calculated:\n";
        std::cout << homography_ << std::endl;
        return 0;
    } else {

        std::cerr << oat::Error("Failed to fit a homography to the data set.\n")
                  << oat::Error("Check the sanity of your data and/or try a different transform estimation method.\n");
        return -1;
    }
}

cv::Mat HomographyGenerator::drawMousePoint(cv::Mat& frame) {

    // Write the click point coords on the frame
    cv::circle(frame, mouse_pt_, 2, cv::Scalar(0, 0, 255), -1);
    std::string coord = "(" + std::to_string(mouse_pt_.x) +
                        ", " + std::to_string(mouse_pt_.y) + ")";
    cv::Point coord_text_origin(mouse_pt_.x + 10.0, mouse_pt_.y + 10.0);
    cv::putText(frame, coord, coord_text_origin, 1, 1, cv::Scalar(0, 0, 255));

    // If homography is valid, also show the transform coordinates
    if (homography_valid_) {

        std::vector<cv::Point2f> q_world;
        std::vector<cv::Point2f> q_camera {mouse_pt_};
        cv::perspectiveTransform(q_camera, q_world, homography_);

        std::string coord = "(" + std::to_string(q_world[0].x) +
                ", " + std::to_string(q_world[0].y) + ")";

        cv::Point coord_text_origin(mouse_pt_.x + 10.0, mouse_pt_.y - 10.0);
        cv::putText(frame, coord, coord_text_origin, 1, 1, cv::Scalar(0, 0, 255));
    }

    return frame;
}

void HomographyGenerator::onMouseEvent(int event, int x, int y, int, void* _this) {

    static_cast<HomographyGenerator*>(_this)->onMouseEvent(event, x, y);
}

void HomographyGenerator::onMouseEvent(int event, int x, int y) {

    if (event == cv::EVENT_LBUTTONDOWN) {
        mouse_pt_.x = x;
        mouse_pt_.y = y;

        clicked_ = true;
    }
}

} /* namespace oat */
