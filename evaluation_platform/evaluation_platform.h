/*
 * evaluation_platform.h
 *
 *  Created on: Feb 15, 2017
 *      Author: kevin
 */


#include <vector>

#include <gazebo/physics/physics.hh>
#include <gazebo/common/common.hh>
#include <gazebo/gazebo.hh>

#include "/home/kevin/research/gazebo/msgs/include/pose_estimation_result.pb.h"

#include "evaluation_criteria.h"

using namespace std;
using namespace gazebo;

typedef const boost::shared_ptr<const my::msgs::PoseEstimationResult > ConstMsgsPoseEstimationResultPtr;
typedef const boost::shared_ptr<const gazebo::msgs::Request > ConstMsgsRequestPtr;

class EvaluationPlatform : public WorldPlugin
{

public:
	void Load( physics::WorldPtr _parent, sdf::ElementPtr /*_sdf*/ );


private:
	void _onUpdate( const common::UpdateInfo & /*_info*/ );
	// callback function of algorithm's result
	void _receiveResult( ConstMsgsPoseEstimationResultPtr &_msg );

	void _receiveEnded( ConstMsgsRequestPtr &_msg );

	void _environmentConstruction();

	void _throwObjects();

	// for only snapshot mode
	void _rethrowForOnlySnapshot( ConstMsgsRequestPtr &_msgs );

	// visualize the result of the Algorithm
	void _resultVisualize( const int _model_idx, const math::Pose _pose );

	// initialize parameters, including load parameters from xml file
	void _initParameters( const std::string &_filename );

	void _initLog( const std::string &_param_filename );

	void _optimizePathFromXML( string &_path );

private:
    // Pointer to the model
    physics::WorldPtr m_world;

    // Pointer to the update event connection
    event::ConnectionPtr m_update_connection;

    // contain the name of every target models
    std::vector< std::string > m_models_name;

    // whether the model is being estimated, in the same order of m_models_name
    std::vector< bool > m_estimated_models;

    // store the sensor pose at the time when sensor take picture
    math::Matrix4 m_sensor_pose;

    // store 'World to Camera Matrix' at the time when sensor take picture
    math::Matrix4 m_wld_to_cam_mat;




    // transport::Node
	transport::NodePtr m_node_ptr;

	// transport::Publisher for request sensor to take picture
	transport::PublisherPtr m_publisher_ptr;

	// transport::Publisher for re-simulate request
	transport::PublisherPtr m_resimulate_publisher_ptr;

	// transport::Publisher for evaluation result
	transport::PublisherPtr m_evaluation_result_publisher_ptr;

	// transport::Publisher for only snapshot
	transport::PublisherPtr m_snapshot_publisher_ptr;

	// transport::Subscriber to subscribe pose estimation result message
	transport::SubscriberPtr m_subscriber_ptr;

	// transport::Subscriber to subscribe pose estimation ended message
	transport::SubscriberPtr m_ended_subscriber_ptr;

	// transport::Subscriber to subscribe rethrow event from depth sensor
	transport::SubscriberPtr m_rethrow_subscriber_ptr;

	// ********************************** //
	// parameters - evaluation attributes //
	// ********************************** //
	bool m_resimulate_after_fail;

	// ****************************************************** //
	// parameters - evaluation attributes for multiple models //
	// ****************************************************** //
	vector< EvaluationCriteria > m_criteria;

	// ******************************** //
	// parameters - stacking parameters //
	// ******************************** //

	// parameters for only snapshot
	int m_snapshot_mode;
	int m_total_snapshot;
	int m_current_snapshot;

	string m_box_model_sdf_file_path;
	// box size
	math::Vector3 m_box_size;
	// box wall thickness
	float m_box_wall_thickness;
	// file path to target model SDF file
	vector< string > m_target_model_sdf_file_paths;
	// extracted from target model SDF file
	vector< string > m_target_model_names;
	// target models propotion
	vector< int > m_target_model_proportions;
	// check whether target models are steady every m_check_steady_interval secs
	double m_check_steady_interval;
	// stop objects and start estimate after m_consecutive_steady_threshold
	int m_consecutive_steady_threshold;
	// linear velocity steady threshold
	double m_linear_vel_threshold;
	int m_stacking_width;		// better be odd number
	int m_stacking_height;	// better be odd number
	int m_stacking_layers;
	float m_stacking_distance;
	float m_throwing_height;

	gazebo::math::Vector3 m_box_center;
	gazebo::math::Vector3 m_stacking_center;

	// ***************** //
	// parameters - log //
	// ***************** //
	std::string m_log_directory;
	bool m_error_logging;
	bool m_success_logging;

	// ************ //
	// global flags //
	// ************ //
	bool m_rethrowed;
	bool m_inestimable_state;
	bool m_skip_receive_result;		// due to unexpect behavior of Subscriber::Unsubscribe

};
// Register this plugin with the simulator
GZ_REGISTER_WORLD_PLUGIN( EvaluationPlatform )
