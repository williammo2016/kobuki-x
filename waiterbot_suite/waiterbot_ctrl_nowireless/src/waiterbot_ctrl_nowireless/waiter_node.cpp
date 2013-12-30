/*
  WaiterIsolated Class

  LICENSE : BSD - https://raw.github.com/yujinrobot/kobuki-x/license/LICENSE

  Author :  Jihoon Lee
  Date   : Dec 2013
 */

#include "waiterbot_ctrl_nowireless/waiter_node.hpp"

namespace waiterbot {
  WaiterIsolated::WaiterIsolated(ros::NodeHandle& n) : 
    nh_(n), 
    navigator_(n),
    watchdog_(n),
    ac_autodock_(WaiterIsolatedDefaultParam::AC_AUTODOCK, true)
  {  
    initialized_ = false;
    waypointsReceived_ = false;
    inCommand_= false;
    digital_input_first_time_ = false;
    init();
  }

  WaiterIsolated::~WaiterIsolated() {  }

  void WaiterIsolated::init()
  {
    // parameters 
    ros::NodeHandle priv_n("~");

    priv_n.param("loc_vm",        loc_vm_,       WaiterIsolatedDefaultParam::LOC_VM);
    priv_n.param("loc_customer",  loc_customer_, WaiterIsolatedDefaultParam::LOC_CUSTOMER);
    priv_n.param("base_frame",    base_frame_,   WaiterIsolatedDefaultParam::BASE_FRAME);
    priv_n.param("odom_frame",    odom_frame_,   WaiterIsolatedDefaultParam::ODOM_FRAME);
    priv_n.param("global_frame",  global_frame_, WaiterIsolatedDefaultParam::GLOBAL_FRAME);

    // listen to green and red buttons
    sub_digital_input_ = nh_.subscribe(WaiterIsolatedDefaultParam::SUB_DIGITAL_INPUT, 5, & WaiterIsolated::digitalInputCB, this);

    // listen to waypoints
    sub_waypoints_ = nh_.subscribe(WaiterIsolatedDefaultParam::SUB_WAYPOINTS, 5, &WaiterIsolated::waypointsCB, this);
    // listen to drink order message
    sub_navctrl_ = nh_.subscribe(WaiterIsolatedDefaultParam::SUB_DRINK_ORDER, 1, &WaiterIsolated::commandCB, this);

    // feedback to tablet 
    pub_navctrl_feedback_= nh_.advertise<waiterbot_msgs::NavCtrlStatus>(WaiterIsolatedDefaultParam::PUB_DRINK_ORDER_FEEDBACK, 1);
  }

  bool WaiterIsolated::isInit() {
    if(waypointsReceived_ && navigator_.isInit())
      return true;
    else
      return false;
  }

  void WaiterIsolated::digitalInputCB(const kobuki_msgs::DigitalInputEvent::ConstPtr& msg)
  {
    if(digital_input_first_time_ == false)
    {
      prev_digital_input = *msg;
      digital_input_first_time_ = true;
    }
    else {

      // only case whn the red button is pressed while delivery
      if(prev_digital_input.values[1] == false && msg->values[1] == true && inCommand_ == true) 
      {
        ROS_WARN("Cancel button pressed. Canceling delivery....");
        navigator_.cancelMoveTo();
        sendFeedback(waiterbot_msgs::NavCtrlStatus::CANCEL, std::string("Cancel button pressed. Canceling all commands."));
      }

      if(prev_digital_input.values[1] == false && msg->values[1] == true)
      {
        ROS_INFO("Waiter : Red button pressed");
      }

      if(prev_digital_input.values[0] == false && msg->values[0] == true)
      {
        ROS_INFO("Waiter : Green button pressed");
      }

      // ignore any other cases
      prev_digital_input = *msg;
    }
  }

  void WaiterIsolated::waypointsCB(const yocs_msgs::WaypointList::ConstPtr& msg)
  {
    unsigned int i;

    // wait until it finishes a delivery
    while(inCommand_) { ros::Duration(1).sleep(); }

    map_wp_.clear();
    for(i = 0; i < msg->waypoints.size(); i++)
    {
      geometry_msgs::PoseStamped ps;

      ps.header = msg->waypoints[i].header;
      ps.pose = msg->waypoints[i].pose;

      map_wp_[msg->waypoints[i].name] = ps;
    }
    
    ROS_INFO("Received %lu waypoints", map_wp_.size());
    waypointsReceived_ = true;
  }

  void WaiterIsolated::commandCB(const waiterbot_msgs::NavCtrlGoTo::ConstPtr& msg)
  {
    ROS_INFO("Waiter : Command Received %d",msg->goal);

    if(inCommand_)
    {
      ROS_WARN("Waiter : It is serving a command already. rejecting...");
      sendFeedback(waiterbot_msgs::NavCtrlStatus::ERROR, std::string("It is serving a command already. rejecting..."));
      return;
    }
    else {
      sendFeedback(waiterbot_msgs::NavCtrlStatus::ACCEPTED, std::string(""));
      inCommand_= true;
    }

    // starts to serve.
    ROS_INFO("Starting work...");
    command_process_thread_ = boost::thread(&WaiterIsolated::processCommand, this, msg->goal);
  }

  bool WaiterIsolated::endCommand(const int feedback, const std::string message)
  {
    sendFeedback(feedback, message);
    inCommand_ = false;
    return true;
  }

  void WaiterIsolated::sendFeedback(const int feedback, const std::string message)
  {
    waiterbot_msgs::NavCtrlStatus fd;

    fd.status_code = feedback;
    fd.status_message = message;

    pub_navctrl_feedback_.publish(fd);
  }

  void WaiterIsolated::spin() 
  {
    ros::spin();
  }
}