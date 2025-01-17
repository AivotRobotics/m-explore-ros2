#include <explore/costmap_tools.h>
#include <explore/frontier_search.h>

#include <geometry_msgs/msg/point.hpp>
#include <mutex>

#include "nav2_costmap_2d/cost_values.hpp"

namespace frontier_exploration
{
using nav2_costmap_2d::FREE_SPACE;
using nav2_costmap_2d::LETHAL_OBSTACLE;
using nav2_costmap_2d::NO_INFORMATION;

FrontierSearch::FrontierSearch(nav2_costmap_2d::Costmap2D* costmap,
                               double potential_scale, double gain_scale, double orientation_scale,
                               double min_frontier_size, double max_frontier_size)
  : logger_(rclcpp::get_logger(__func__))
  , costmap_(costmap)
  , potential_scale_(potential_scale)
  , gain_scale_(gain_scale)
  , orientation_scale_(orientation_scale)
  , min_frontier_size_(min_frontier_size)
  , max_frontier_size_(max_frontier_size)
{
}

std::vector<Frontier>
FrontierSearch::searchFrom(const geometry_msgs::msg::Pose& pose)
{
  std::vector<Frontier> frontier_list;

  // Sanity check that robot is inside costmap bounds before searching
  unsigned int mx, my;
  if (!costmap_->worldToMap(pose.position.x, pose.position.y, mx, my)) {
    RCLCPP_ERROR(logger_, "Robot out of costmap bounds, cannot search for frontiers");
    return frontier_list;
  }

  // make sure map is consistent and locked for duration of search
  std::lock_guard<nav2_costmap_2d::Costmap2D::mutex_t> lock(
      *(costmap_->getMutex()));

  map_ = costmap_->getCharMap();
  size_x_ = costmap_->getSizeInCellsX();
  size_y_ = costmap_->getSizeInCellsY();

  // initialize flag arrays to keep track of visited and frontier cells
  std::vector<bool> frontier_flag(size_x_ * size_y_, false);
  std::vector<bool> visited_flag(size_x_ * size_y_, false);

  // initialize breadth first search
  std::queue<unsigned int> bfs;

  // find closest clear cell to start search
  unsigned int clear, pos = costmap_->getIndex(mx, my);
  if (nearestCell(clear, pos, FREE_SPACE, *costmap_)) {
    bfs.push(clear);
  } else {
    bfs.push(pos);
    RCLCPP_WARN(logger_, "Could not find nearby clear cell to start search");
  }
  visited_flag[bfs.front()] = true;

  while (!bfs.empty()) {
    unsigned int idx = bfs.front();
    bfs.pop();

    // iterate over 4-connected neighbourhood
    for (unsigned nbr : nhood4(idx, *costmap_)) {
      // add to queue all free, unvisited cells, use descending search in case
      // initialized on non-free cell
      if (map_[nbr] <= map_[idx] && !visited_flag[nbr]) {
        visited_flag[nbr] = true;
        bfs.push(nbr);
        // check if cell is new frontier cell (unvisited, NO_INFORMATION, free
        // neighbour)
      } else if (isNewFrontierCell(nbr, frontier_flag)) {
        frontier_flag[nbr] = true;
        Frontier new_frontier = buildNewFrontier(nbr, pose, frontier_flag);
        if (new_frontier.size * costmap_->getResolution() >= min_frontier_size_) {

          new_frontier.cost = frontierCost(new_frontier);
          frontier_list.push_back(new_frontier);
        }
      }
    }
  }

  std::sort(
      frontier_list.begin(), frontier_list.end(),
      [](const Frontier& f1, const Frontier& f2) { return f1.cost < f2.cost; });

  return frontier_list;
}

Frontier FrontierSearch::buildNewFrontier(unsigned int initial_cell,
                                          const geometry_msgs::msg::Pose& reference_pose,
                                          std::vector<bool>& frontier_flag)
{
  // initialize frontier structure
  Frontier output;
  output.centroid.x = 0;
  output.centroid.y = 0;
  output.size = 1;
  output.min_distance = std::numeric_limits<double>::infinity();

  // record initial contact point for frontier
  unsigned int ix, iy;
  costmap_->indexToCells(initial_cell, ix, iy);
  costmap_->mapToWorld(ix, iy, output.initial.x, output.initial.y);

  // push initial gridcell onto queue
  std::queue<unsigned int> bfs;
  bfs.push(initial_cell);

  bool frontier_completed = false;
  while (!frontier_completed) {
    // if no more cells to check than we are done building a frontier
    if (bfs.empty()) {
      frontier_completed = true;
      continue;
    }

    unsigned int idx = bfs.front();
    bfs.pop();

    // try adding cells in 8-connected neighborhood to frontier
    for (unsigned int nbr : nhood8(idx, *costmap_)) {
      // check if neighbour is a potential frontier cell
      if (isNewFrontierCell(nbr, frontier_flag)) {
        // mark cell as frontier
        frontier_flag[nbr] = true;
        unsigned int mx, my;
        double wx, wy;
        costmap_->indexToCells(nbr, mx, my);
        costmap_->mapToWorld(mx, my, wx, wy);

        geometry_msgs::msg::Point point;
        point.x = wx;
        point.y = wy;
        output.points.push_back(point);

        // update frontier size
        output.size++;

        // update centroid of frontier
        output.centroid.x += wx;
        output.centroid.y += wy;

        // determine frontier's distance from robot
        const auto distance = std::sqrt(
          std::pow(reference_pose.position.x - wx, 2.0) + std::pow(reference_pose.position.y - wy, 2.0));
        if (distance < output.min_distance) {
          output.min_distance = distance;
          output.middle.x = wx;
          output.middle.y = wy;
        }

        // if max frontier size is set and the frontier exceeds it then stop iterating over cells
        if (max_frontier_size_ > 0.0 && output.size * costmap_->getResolution() >= max_frontier_size_) {
          frontier_completed = true;
          break;
        }

        // add to queue for breadth first search
        bfs.push(nbr);
      }
    }
  }

  // average out frontier centroid
  output.centroid.x /= output.size;
  output.centroid.y /= output.size;

  // we want to use middle point as frontier goal
  // make it this way but need to replace centroid usage
  // across the codebase
  output.centroid = output.middle;

  // update frontier orientation relative to current robot position
  const auto front_rx = output.middle.x - reference_pose.position.x;
  const auto front_ry = output.middle.y - reference_pose.position.y;

  // angle = atan2(a2*b1 - a1*b2, a1*b1 - a2*b2), where b = X_AXIS = (1, 0)
  const double yaw = std::atan2(front_ry, front_rx);
  const tf2::Quaternion orient_out { { 0, 0, 1 }, yaw };
  output.orientation = toMsg(orient_out);

  // calc angular distance
  tf2::Quaternion orient_curr;
  tf2::fromMsg(reference_pose.orientation, orient_curr);
  output.angular_distance = std::abs(orient_curr.angleShortestPath(orient_out));

  return output;
}

bool FrontierSearch::isNewFrontierCell(unsigned int idx,
                                       const std::vector<bool>& frontier_flag)
{
  // check that cell is unknown and not already marked as frontier
  if (map_[idx] != NO_INFORMATION || frontier_flag[idx]) {
    return false;
  }

  // frontier cells should have at least one cell in 4-connected neighbourhood
  // that is free
  for (unsigned int nbr : nhood4(idx, *costmap_)) {
    if (map_[nbr] == FREE_SPACE) {
      return true;
    }
  }

  return false;
}

double FrontierSearch::frontierCost(const Frontier& frontier) const {
  const auto position = potential_scale_ * frontier.min_distance * costmap_->getResolution();
  const auto gain = gain_scale_ * frontier.size * costmap_->getResolution();
  const auto orientation = orientation_scale_ * frontier.angular_distance;

  return orientation + position - gain;
}
}  // namespace frontier_exploration
