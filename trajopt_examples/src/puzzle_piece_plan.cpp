#include <ros/ros.h>
#include <tesseract_ros/bullet/bullet_env.h>
#include <tesseract_ros/kdl/kdl_chain_kin.h>
#include <trajopt/problem_description.hpp>
#include <trajopt/plot_callback.hpp>
#include <trajopt_utils/logging.hpp>
#include <trajopt_utils/config.hpp>
#include <urdf_parser/urdf_parser.h>
#include <srdfdom/model.h>

// For loading the pose file from a local package
#include <ros/package.h>
#include <fstream>

using namespace trajopt;
using namespace tesseract;

const std::string ROBOT_DESCRIPTION_PARAM = "robot_description"; /**< Default ROS parameter for robot description */
const std::string ROBOT_SEMANTIC_PARAM = "robot_description_semantic"; /**< Default ROS parameter for robot description */

bool plotting_ = false;
urdf::ModelInterfaceSharedPtr model_;  /**< URDF Model */
srdf::ModelSharedPtr srdf_model_;      /**< SRDF Model */
tesseract_ros::BulletEnvPtr env_;      /**< Trajopt Basic Environment */

static EigenSTL::vector_Affine3d makePuzzleToolPoses()
{
  EigenSTL::vector_Affine3d path; // results
  std::ifstream indata; // input file

  // You could load your parts from anywhere, but we are transporting them with the git repo
  std::string filename = ros::package::getPath("trajopt_examples") + "/config/puzzle_bent.csv";

  // In a non-trivial app, you'll of course want to check that calls like 'open' succeeded
  indata.open(filename);

  std::string line;
  int lnum = 0;
  while (std::getline(indata, line))
  {
      ++lnum;
      if (lnum < 3)
        continue;

      std::stringstream lineStream(line);
      std::string  cell;
      Eigen::Matrix<double, 6, 1> xyzijk;
      int i = -2;
      while (std::getline(lineStream, cell, ','))
      {
        ++i;
        if (i == -1)
          continue;

        xyzijk(i) = std::stod(cell);
      }

      Eigen::Vector3d pos = xyzijk.head<3>();
      pos = pos / 1000.0; // Most things in ROS use meters as the unit of length. Our part was exported in mm.
      Eigen::Vector3d norm = xyzijk.tail<3>();
      norm.normalize();

      // This code computes two extra directions to turn the normal direction into a full defined frame. Descartes
      // will search around this frame for extra poses, so the exact values do not matter as long they are valid.
      Eigen::Vector3d temp_x = (-1 * pos).normalized();
      Eigen::Vector3d y_axis = (norm.cross(temp_x)).normalized();
      Eigen::Vector3d x_axis = (y_axis.cross(norm)).normalized();
      Eigen::Affine3d pose;
      pose.matrix().col(0).head<3>() = x_axis;
      pose.matrix().col(1).head<3>() = y_axis;
      pose.matrix().col(2).head<3>() = norm;
      pose.matrix().col(3).head<3>() = pos;

      path.push_back(pose);
  }
  indata.close();

  return path;
}

ProblemConstructionInfo cppMethod()
{
  ProblemConstructionInfo pci(env_);

  EigenSTL::vector_Affine3d tool_poses = makePuzzleToolPoses();

  // Populate Basic Info
  pci.basic_info.n_steps = tool_poses.size();
  pci.basic_info.manip = "manipulator";
  pci.basic_info.start_fixed = false;

  pci.opt_info.max_iter = 200;
  pci.opt_info.min_approx_improve = 1e-3;
  pci.opt_info.min_trust_box_size = 1e-3;

  // Create Kinematic Object
  pci.kin = pci.env->getManipulator(pci.basic_info.manip);

  // Populate Init Info
  Eigen::VectorXd start_pos = pci.env->getCurrentJointValues(pci.kin->getName());


  pci.init_info.type = InitInfo::GIVEN_TRAJ;
  pci.init_info.data = start_pos.transpose().replicate(pci.basic_info.n_steps, 1);
//  pci.init_info.data.col(6) = VectorXd::LinSpaced(steps_, start_pos[6], end_pos[6]);


  // Populate Cost Info
  boost::shared_ptr<JointVelCostInfo> joint_vel = boost::shared_ptr<JointVelCostInfo>(new JointVelCostInfo);
  joint_vel->coeffs = std::vector<double>(7, 1.0);
  joint_vel->name = "joint_vel";
  joint_vel->term_type = TT_COST;
  pci.cost_infos.push_back(joint_vel);

  boost::shared_ptr<JointAccCostInfo> joint_acc = boost::shared_ptr<JointAccCostInfo>(new JointAccCostInfo);
  joint_acc->coeffs = std::vector<double>(7, 2.0);
  joint_acc->name = "joint_acc";
  joint_acc->term_type = TT_COST;
  pci.cost_infos.push_back(joint_acc);

  boost::shared_ptr<JointJerkCostInfo> joint_jerk = boost::shared_ptr<JointJerkCostInfo>(new JointJerkCostInfo);
  joint_jerk->coeffs = std::vector<double>(7, 5.0);
  joint_jerk->name = "joint_jerk";
  joint_jerk->term_type = TT_COST;
  pci.cost_infos.push_back(joint_jerk);

  boost::shared_ptr<CollisionCostInfo> collision = boost::shared_ptr<CollisionCostInfo>(new CollisionCostInfo);
  collision->name = "collision";
  collision->term_type = TT_COST;
  collision->continuous = false;
  collision->first_step = 0;
  collision->last_step = pci.basic_info.n_steps - 1;
  collision->gap = 1;
  collision->info = createSafetyMarginDataVector(pci.basic_info.n_steps, 0.025, 20);
  pci.cost_infos.push_back(collision);

  // Populate Constraints
  Eigen::Affine3d grinder_frame = env_->getLinkTransform("grinder_frame");
  Eigen::Quaterniond q(grinder_frame.linear());

  Eigen::Vector3d stationary_xyz = grinder_frame.translation();
  Eigen::Vector4d stationary_wxyz = Eigen::Vector4d(q.w(), q.x(), q.y(), q.z());

  for (auto i = 0; i < pci.basic_info.n_steps; ++i)
  {
    boost::shared_ptr<PoseCostInfo> pose = boost::shared_ptr<PoseCostInfo>(new PoseCostInfo);
    pose->term_type = TT_CNT;
    pose->name = "waypoint_cart_" + std::to_string(i);
    pose->link = "part";
    pose->tcp = tool_poses[i];
    pose->timestep = i;
    pose->xyz = stationary_xyz;
    pose->wxyz = stationary_wxyz;
    pose->pos_coeffs = Eigen::Vector3d(10, 10, 10);
    pose->rot_coeffs = Eigen::Vector3d(10, 10, 0);

    pci.cnt_infos.push_back(pose);
  }

  return pci;
}


int main(int argc, char** argv)
{
  ros::init(argc, argv, "puzzle_piece_plan");
  ros::NodeHandle pnh("~");
  ros::NodeHandle nh;

  // Initial setup
  std::string urdf_xml_string, srdf_xml_string;
  nh.getParam(ROBOT_DESCRIPTION_PARAM, urdf_xml_string);
  nh.getParam(ROBOT_SEMANTIC_PARAM, srdf_xml_string);

  model_ = urdf::parseURDF(urdf_xml_string);
  srdf_model_ = srdf::ModelSharedPtr(new srdf::Model);
  srdf_model_->initString(*model_, srdf_xml_string);
  env_ = tesseract_ros::BulletEnvPtr(new tesseract_ros::BulletEnv);
  assert(model_ != nullptr);
  assert(env_ != nullptr);

  bool success = env_->init(model_, srdf_model_);
  assert(success);

  // Get ROS Parameters
  pnh.param("plotting", plotting_, plotting_);

  // Set the robot initial state
  std::unordered_map<std::string, double> ipos;
  ipos["joint_a1"] = -0.785398;
  ipos["joint_a2"] = 0.4;
  ipos["joint_a3"] = 0.0;
  ipos["joint_a4"] = -1.9;
  ipos["joint_a5"] = 0.0;
  ipos["joint_a6"] = 1.0;
  ipos["joint_a7"] = 0.0;
  env_->setState(ipos);

  // Set Log Level
  gLogLevel = util::LevelInfo;

  // Setup Problem
  ProblemConstructionInfo pci = cppMethod();
  TrajOptProbPtr prob = ConstructProblem(pci);

  // Solve Trajectory
  ROS_INFO("puzzle piece plan");

  tesseract::DistanceResultVector collisions;
  const std::vector<std::string>& joint_names = prob->GetKin()->getJointNames();
  const std::vector<std::string>& link_names = prob->GetKin()->getLinkNames();

  env_->continuousCollisionCheckTrajectory(joint_names, link_names, prob->GetInitTraj(), collisions);
  ROS_INFO("Initial trajector number of continuous collisions: %lui\n", collisions.size());

  BasicTrustRegionSQP opt(prob);
  opt.setParameters(pci.opt_info);
  if (plotting_)
  {
    opt.addCallback(PlotCallback(*prob));
  }

  opt.initialize(trajToDblVec(prob->GetInitTraj()));
  ros::Time tStart = ros::Time::now();
  sco::OptStatus status = opt.optimize();
  ROS_INFO("Optimization Status: %s, Planning time: %.3f", sco::statusToString(status).c_str(), (ros::Time::now() - tStart).toSec());

  if (plotting_)
  {
    prob->GetEnv()->plotClear();
  }

  // Plot the final trajectory
  env_->plotTrajectory("", joint_names, getTraj(opt.x(), prob->GetVars()));

  collisions.clear();
  env_->continuousCollisionCheckTrajectory(joint_names, link_names, getTraj(opt.x(), prob->GetVars()), collisions);
  ROS_INFO("Final trajectory number of continuous collisions: %lui\n", collisions.size());

}
