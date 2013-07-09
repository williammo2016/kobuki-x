/*
 * virtual_sensor_node.cpp
 *
 *  Created on: May 13, 2013
 *      Author: jorge
 */

#include "virtual_sensor/virtual_sensor_node.hpp"


namespace virtual_sensor
{

VirtualSensorNode::VirtualSensorNode()
{
}

VirtualSensorNode::~VirtualSensorNode()
{
}


bool VirtualSensorNode::init()
{
  ros::NodeHandle nh;
  ros::NodeHandle pnh("~");

  int samples;

  pnh.param("samples",    samples,    360);
  pnh.param("angle_min",  angle_min_, -M_PI);
  pnh.param("angle_max",  angle_max_, +M_PI);
  pnh.param("range_min",  range_min_, 0.0);
  pnh.param("range_max",  range_max_, std::numeric_limits<double>::quiet_NaN());
  pnh.param("frequency",  frequency_, std::numeric_limits<double>::quiet_NaN());
  pnh.param("hits_count", hits_count_, 2);
  pnh.param("sensor_frame", sensor_frame_id_, std::string("/base_link"));
  pnh.param("global_frame", global_frame_id_, std::string("/map"));

  double costmap_update_frequency, costmap_width, costmap_height;

  nh.getParam("move_base/local_costmap/update_frequency", costmap_update_frequency);
  nh.getParam("move_base/local_costmap/width",            costmap_width);
  nh.getParam("move_base/local_costmap/height",           costmap_height);

  // Use local costmap configuration to decide some default values
  if (std::isnan(range_max_) == true)
    range_max_ = std::max(costmap_width, costmap_height)/2.0;

  if (std::isnan(frequency_) == true)
    frequency_ = costmap_update_frequency*2.0;

  column_poses_sub_ = nh.subscribe("column_pose_list", 1, &VirtualSensorNode::columnPosesCB, this);
  wall_poses_sub_   = nh.subscribe("wall_pose_list",   1, &VirtualSensorNode::wallPosesCB, this);
  table_poses_sub_  = nh.subscribe("table_pose_list",  1, &VirtualSensorNode::tablePosesCB, this);
  virtual_obs_pub_  = nh.advertise <sensor_msgs::LaserScan> ("virtual_sensor_scan", 1, true);

  // Set virtual scan configuration
  scan_.header.frame_id = sensor_frame_id_;
  scan_.angle_min = angle_min_;
  scan_.angle_max = angle_max_;
  scan_.angle_increment = (angle_max_ - angle_min_)/samples;
  scan_.scan_time = 1.0 / frequency_;
  scan_.range_min = range_min_;
  scan_.range_max = range_max_;
  scan_.ranges.resize(samples);

  return true;
}


void VirtualSensorNode::tablePosesCB(const semantic_region_handler::TablePoseList::ConstPtr& msg)
{
  // Just take first message; ignore the rest, as table list is not dynamic
  if ((circles_.size() == 0) && (msg->tables.size() > 0))
  {
    for (unsigned int i = 0; i < msg->tables.size(); i++)
    {
      // Skip the pickup point
      if (msg->tables[i].name.find("pickup") == std::string::npos)
        circles_.push_back(msg->tables[i]);
    }
  }
}

void VirtualSensorNode::columnPosesCB(const virtual_sensor::ColumnList::ConstPtr& msg)
{
  columns_ = msg->obstacles;
}

void VirtualSensorNode::wallPosesCB(const virtual_sensor::WallList::ConstPtr& msg)
{
  // Just take first message; ignore the rest, as table list is not dynamic
//  if ((walls_.size() == 0) && (msg->obstacles.size() > 0))
//  {
//    for (unsigned int i = 0; i < msg->obstacles.size(); i++)
//    {
//      walls_.push_back(msg->obstacles[i]);
//    }
//  }
  walls_ = msg->obstacles;
}


void VirtualSensorNode::spin()
{
  ros::Rate rate(frequency_);

  while (ros::ok())
  {
    if ((virtual_obs_pub_.getNumSubscribers() > 0) && ((circles_.size() > 0) || (walls_.size() > 0)))
    {
      scan_.header.stamp = ros::Time::now();

      tf::StampedTransform robot_gb;
      try
      {
        tf_listener_.lookupTransform(global_frame_id_, sensor_frame_id_, ros::Time(0.0), robot_gb);
      }
      catch (tf::TransformException& e)
      {
        // This can happen during startup, while some part of the localization chain is missing
        // If not, probably sensor_frame is missconfigured
        ROS_WARN_THROTTLE(2, "Cannot get tf %s -> %s: %s",
                          global_frame_id_.c_str(), sensor_frame_id_.c_str(), e.what());
        continue;
      }
      tf::Transform robot_gb_inv = robot_gb.inverse();

      // Pre-filter obstacles:
      //  - put in robot reference system
      //  - remove those out of range
      //  - short by increasing distance to the robot
      std::vector< boost::shared_ptr<Obstacle> > obstacles;
      for (unsigned int i = 0; i < circles_.size(); i++)
      {
        tf::Transform obs_abs_tf = tf::Transform(tf::Quaternion::getIdentity(),
                                                 tf::Vector3(circles_[i].pose_cov_stamped.pose.pose.position.x,
                                                             circles_[i].pose_cov_stamped.pose.pose.position.y,
                                                             0.0));
        tf::Transform obs_tf = robot_gb_inv * obs_abs_tf;
        boost::shared_ptr<Obstacle> new_obs(new Circle(obs_tf, circles_[i].radius));

        add(new_obs, obstacles);
      }

      for (unsigned int i = 0; i < walls_.size(); i++)
      {
        tf::Transform obs_abs_tf;
        tf::poseMsgToTF(walls_[i].pose.pose.pose, obs_abs_tf);
//        tf::Vector3 p1, p2;
//        tf::pointMsgToTF(walls_[i].p1, p1);
//        tf::pointMsgToTF(walls_[i].p2, p2);
//        tf::Transform p1_abs_tf = tf::Transform(tf::Quaternion::getIdentity(), p1);
//        tf::Transform p2_abs_tf = tf::Transform(tf::Quaternion::getIdentity(), p2);
        tf::Transform obs_tf = robot_gb_inv * obs_abs_tf;
        boost::shared_ptr<Obstacle> new_obs(new Wall(obs_tf, walls_[i].length, walls_[i].width, walls_[i].height));

        add(new_obs, obstacles);
      }

      int ray = 0;

      // Fire sensor "rays" and register closest hit, if any. We test two consecutive objects, what in most cases will
      // be enough, although in some cases three or more ill-arranged obstacles will return a wrong shortest distance
      // (remember that obstacles are shorted by its closest distance to the robot)
      for (double ray_theta = scan_.angle_min; ray_theta <= scan_.angle_max; ray_theta += scan_.angle_increment)
      {
        double rx = scan_.range_max*std::cos(ray_theta);
        double ry = scan_.range_max*std::sin(ray_theta);

        int hits = 0;
        scan_.ranges[ray] = scan_.range_max;

        for (unsigned int i = 0; i < obstacles.size(); i++)
        {
          double distance;
          if (obstacles[i]->intersects(rx, ry, scan_.range_max, distance) == true)
          {
            // Hit; take the shortest distance until now and keep checking until we reach hits_count_
            scan_.ranges[ray] = std::min(scan_.ranges[ray], (float)distance);

            if (hits < hits_count_)
            {
              hits++;
            }
            else
            {
              break;   // enough hits
            }
          }
        }

        ray++;
      }

      virtual_obs_pub_.publish(scan_);
    }

    ros::spinOnce();
    rate.sleep();
  }
}

bool VirtualSensorNode::add(boost::shared_ptr<Obstacle>& new_obs, std::vector< boost::shared_ptr<Obstacle> >& obstacles)
{
  if (new_obs->distance() <= 0.0)
  {
    ROS_WARN_THROTTLE(2, "The robot is inside an obstacle??? Ignore it (distance: %f)", new_obs->distance());
    return false;
  }

  if (new_obs->distance() <= scan_.range_max)
  {
    std::vector< boost::shared_ptr<Obstacle> >::iterator it;
    for (it = obstacles.begin(); it != obstacles.end(); ++it)
    {
      if (new_obs->distance() <= (*it)->distance())
      {
        obstacles.insert(it, new_obs);
        return true;
      }
    }

    if ((obstacles.size() == 0) || it == obstacles.end())
    {
      // This obstacle is the first inserted or is the farthest from the robot
      obstacles.push_back(new_obs);
      return true;
    }
  }

  return false;
}

} // namespace virtual_sensor


int main(int argc, char **argv)
{
  ros::init(argc, argv, "virtual_sensor");

  virtual_sensor::VirtualSensorNode node;
  if (node.init() == false)
  {
    ROS_ERROR("%s initialization failed", ros::this_node::getName().c_str());
    return -1;
  }
  ROS_INFO("%s initialized", ros::this_node::getName().c_str());
  node.spin();

  return 0;
}
