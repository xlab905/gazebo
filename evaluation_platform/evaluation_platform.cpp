/*
 * evaluation_platform.cpp
 *
 *  Created on: Feb 15, 2017
 *      Author: kevin
 */

#include "evaluation_platform.h"

#include <sstream>

#include <gazebo/msgs/request.pb.h>
#include "gazebo/physics/physics.hh"
#include "gazebo/common/common.hh"
#include "gazebo/gazebo.hh"

// for loading parameters from xml
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
// for loading parameters from xml

// for logging to file
#define BOOST_NO_CXX11_SCOPED_ENUMS
#include <boost/filesystem/operations.hpp>
#undef BOOST_NO_CXX11_SCOPED_ENUMS

#include <boost/date_time/posix_time/posix_time.hpp>
// for logging to file

#define COUT_PREFIX "\033[1;33m" << "[EvaluationPlatform] " << "\033[0m"
#define CERR_PREFIX "\033[1;31m" << "[EvaluationPlatform]" << "\033[0m"

void EvaluationPlatform::Load(physics::WorldPtr _world, sdf::ElementPtr /*_sdf*/)
{
	// Store the world pointer
	m_world = _world;

	// Listen to the update event. This event is broadcast every simulation iteration.
	this->m_update_connection = event::Events::ConnectWorldUpdateBegin( boost::bind(&EvaluationPlatform::_onUpdate, this, _1 ) );

	// initialize global flags
	m_rethrowed = true;
	m_inestimable_state = false;
	m_skip_receive_result = false;

	// load parameters
	_initParameters( "parameters.xml" );

	// ********************* //
	// construct environment //
	// ********************* //
	this->_environmentConstruction();

	// initialize logging
	_initLog( "parameters.xml" );

	// ********************************** //
	// setup connection with depth sensor //
	// ********************************** //
	m_node_ptr = transport::NodePtr( new transport::Node() );
	// Initialize the node with the world name
	m_node_ptr->Init( m_world->GetName() );

	m_publisher_ptr = m_node_ptr->Advertise< gazebo::msgs::Request >( "~/evaluation_platform/take_picture_request" );

	m_resimulate_publisher_ptr = m_node_ptr->Advertise< gazebo::msgs::Request >( "~/evaluation_platform/resimulate_request" );

	m_evaluation_result_publisher_ptr = m_node_ptr->Advertise< gazebo::msgs::Request >( "~/evaluation_platform/evaluation_result" );


	m_snapshot_publisher_ptr = m_node_ptr->Advertise< gazebo::msgs::Request >( "~/evaluation_platform/only_snapshot" );

	// ************************************ //
	// setup connection with pose estimator //
	// ************************************ //

	// subscribe to the result of PoseEstimation
	m_subscriber_ptr = m_node_ptr->Subscribe("~/pose_estimation/estimate_result", &EvaluationPlatform::_receiveResult, this);

	// subscribe to the ended signal of PoseEstimation
	m_ended_subscriber_ptr = m_node_ptr->Subscribe("~/pose_estimation/estimation_ended", &EvaluationPlatform::_receiveEnded, this);

	// subscribe to rethrow event from depth sensor in only snapshot mode
	m_rethrow_subscriber_ptr = m_node_ptr->Subscribe("~/depth_sensor/rethrow_event", &EvaluationPlatform::_rethrowForOnlySnapshot, this);

	// print info
	cout << COUT_PREFIX << "Seed : " << gazebo::math::Rand::GetSeed() << endl;
}

void EvaluationPlatform::_onUpdate( const common::UpdateInfo & /*_info*/ )
{
	// steady counter
	static double cur_time = 0.0;
	static int steady_count = 0;

	// start stacking time ( Real Time )
	static double start_stacking_time;
	static bool get_time_stamp = true;
	if( get_time_stamp )
	{
		start_stacking_time = m_world->GetRealTime().Double();
		get_time_stamp = false;
	}

	// check time interval
	if( m_world->GetSimTime().Double() - cur_time > m_check_steady_interval )
	{
		// print current sim time
		cout << COUT_PREFIX << "SimTime: " << m_world->GetSimTime().Double() << endl;

		// reset cur_time
		cur_time = m_world->GetSimTime().Double();

		// check object's movement state
		for( unsigned int i = 0; i < m_models_name.size(); i++ )
		{
			// check if this model have been estimated
			if( m_estimated_models[ i ] )
			{
				continue;
			}

			physics::ModelPtr cur_model = m_world->GetModel( m_models_name[ i ] );

			if( cur_model->GetWorldLinearVel().Distance( 0, 0, 0 ) < m_linear_vel_threshold )
			{
				if( cur_model->GetWorldAngularVel().Distance( 0, 0, 0 ) < 5 )	// deprecated
				{
					//cout << "\033[1;31m" << "object stopped!" << "\033[0m" << endl;;
				}
				else
				{
					cout << m_models_name[ i ] << endl;
					cout << "angular vel : " << cur_model->GetWorldAngularVel() << endl;
					steady_count = 0;
					return;
				}
			}
			else
			{
				cout << m_models_name[ i ] << endl;
				cout << "linear vel : " << cur_model->GetWorldLinearVel().Distance( 0, 0, 0 ) << endl;
				steady_count = 0;
				return;
			}
		}
		// *************************************************** //
		// all objects are in steady state ( below threshold ) //
		// *************************************************** //
		steady_count++;
		if( steady_count >= m_consecutive_steady_threshold )
		{
			// ****************** //
			// get time to steady //
			// ****************** //
			double current_time = m_world->GetRealTime().Double();

			// store time_to_steady only when re-throwing a new pile
			if( m_rethrowed )
			{
				double time_to_steady = current_time - start_stacking_time;

				// save time to steady info
				string out_filename = m_log_directory + "time_to_steady";
				ofstream file;
				file.open( out_filename.c_str(), ios::out | ios::app );
				if( file.is_open() )
				{
					file << time_to_steady << endl;
					file.close();
				}
				else
				{
					cout << CERR_PREFIX << "Unable to open time_to_steady file" << endl;
					exit( -1 );
				}

				m_rethrowed = false;
			}

			// get time stamp at next iteration
			get_time_stamp = true;

			// ***************************** //
			// set stacking models to static //
			// ***************************** //
			cout << COUT_PREFIX << "object stopped!" << endl;

			for( unsigned int i = 0; i < m_models_name.size(); i++ )
			{
				// check if this model have been estimated
				if( m_estimated_models[ i ] )
				{
					continue;
				}

				physics::ModelPtr cur_model = m_world->GetModel( m_models_name[ i ] );
				cur_model->SetEnabled( false );
				cur_model->SetStatic( true );
				cur_model->SetGravityMode( false );
				cur_model->SetLinearVel( math::Vector3( 0, 0, 0 ) );
				cur_model->SetLinearAccel( math::Vector3( 0, 0, 0 ) );
				cur_model->SetAngularVel( math::Vector3( 0, 0, 0 ) );
				cur_model->SetAngularAccel( math::Vector3( 0, 0, 0 ) );

				vector< physics::LinkPtr > links = cur_model->GetLinks();
				for( unsigned int j = 0; j < links.size(); j++ )
				{
					links[ j ]->SetKinematic( true );						// this is important
				}
			}

			// ********************************** //
			// ask depth sensor to take a picture //
			// ********************************** //
			msgs::Request take_pic_request;
			take_pic_request.set_id( 0 );
			take_pic_request.set_request( "take_one_picture" );

			while( !m_publisher_ptr->HasConnections() )
			{
				cout << COUT_PREFIX << "\033[1;31m" << "have no depth sensor connected!" << "\033[0m" << endl;
				gazebo::common::Time::MSleep( 10 );
			}
			cout << COUT_PREFIX << "Take one shot request." << endl;
			m_publisher_ptr->Publish( take_pic_request );

			// for only snapshot mode //
			if(m_snapshot_mode==1)
			{
				msgs::Request only_snapshot;
				only_snapshot.set_id( 1 );
				std::stringstream ss;
				ss << std::to_string(m_total_snapshot) << ' ';

				for( unsigned int i=0; i<m_models_name.size(); i++ )
				{
					math::Pose models_poses = m_world->GetModel( m_models_name[ i ] )->GetWorldPose();
					// obtain models poses in camera coordinate
					models_poses.pos = models_poses.pos - math::Vector3(-0.10664, 0.075, 0.0);
					//std::cout << "models poses in world " << i << " : "<< models_poses.pos << std::endl;
					models_poses.pos.x = models_poses.pos.x * 6001.5;
					models_poses.pos.y = models_poses.pos.y * 6400;
					//std::cout << "models poses in camera " << i << " : " << models_poses.pos << std::endl;
					ss << abs(models_poses.pos.x) << ' ' << abs(models_poses.pos.y) << ' ';
				}
				//std::cout << "total messges : " << ss.str() << std::endl;
				only_snapshot.set_request( "onlysnapshot_mode" );
				only_snapshot.set_data(ss.str());
				m_snapshot_publisher_ptr->Publish( only_snapshot );
			}

			// ****************************************************** //
			// obtain sensor pose at the time the sensor take picture //
			// ****************************************************** //
			math::Pose sensor_model_pose = m_world->GetModel( "depth_sensor" )->GetWorldPose();

			// convert math::Pose to Matrix4
			math::Matrix4 sensor_model_pose_mat = sensor_model_pose.rot.GetAsMatrix4();
			sensor_model_pose_mat.SetTranslate( sensor_model_pose.pos );

			// obtain sensor pose  ( NOTE: m_sensor_pose is different from sensor_model_pose
			m_sensor_pose = sensor_model_pose_mat *
							math::Quaternion( 0, - M_PI / 2, 0 ).GetAsMatrix4() *
							math::Quaternion( 0, 0, - M_PI / 2 ).GetAsMatrix4();

			m_wld_to_cam_mat = m_sensor_pose.Inverse();

			// allow subscriber
			m_skip_receive_result = false;

			// ******************************** //
			// disconnect with WorldUpdateBegin //
			// ******************************** //
			event::Events::DisconnectWorldUpdateBegin( m_update_connection );

			// reset steady counter
			steady_count = 0;
		}
	}
}

void EvaluationPlatform::_receiveResult( ConstMsgsPoseEstimationResultPtr &_msg )
{
	if( m_skip_receive_result )
	{
		return;
	}

	// check data validation
	if( _msg->pose_matrix4_size() != 16 )
	{
		cerr << CERR_PREFIX << "error data_size of Matrix4" << endl;
		return;
	}

	// save and print received matrix
	math::Matrix4 result;

	for( int j = 0; j < 4; j++ )
	{
		for( int i = 0; i < 4; i++ )
		{
			result[j][i] = _msg->pose_matrix4( i + j * 4 );
		}
	}

	// convert millimeter to meter
	result[0][3] *= 0.001;
	result[1][3] *= 0.001;
	result[2][3] *= 0.001;

	// transform result to world coordinates
	math::Matrix4 result_world = result;
	result_world = m_sensor_pose * result_world;
	cout << "------------------------------------------------------------" << endl;
	cout << COUT_PREFIX << "Recognized Object : " << _msg->object_name() << endl;
	cout << COUT_PREFIX << "Pose Estimation Result ( world coordinate ):" << endl;
	cout << result_world << endl;

	// ******************************************* //
	// find nearest object to the estimated result //
	// ******************************************* //
	physics::ModelPtr cur_nearest_object;
	int cur_nearest_idx = -1;
	double translate_error = std::numeric_limits< double >::max();

	for( unsigned int i = 0; i < m_models_name.size(); i++ )
	{
		// check if this model have been estimated
		if( m_estimated_models[ i ] )
		{
			continue;
		}

		physics::ModelPtr cur_model = m_world->GetModel( m_models_name[ i ] );

		double cur_dist = result_world.GetTranslation().Distance( cur_model->GetWorldPose().pos );
		if( cur_dist < translate_error )
		{
			cur_nearest_object = cur_model;
			translate_error = cur_dist;
			cur_nearest_idx = i;
		}
	}

	// handle no more object
	if( !cur_nearest_object )
	{
		return;
	}

	// print out object correspond to estimation target
	cout << COUT_PREFIX << "Nearest Object : " << cur_nearest_object->GetName() << endl;
	math::Pose nearest_model_pose = cur_nearest_object->GetWorldPose();
	math::Matrix4 nearest_model_pose_matrix4 = nearest_model_pose.rot.GetAsMatrix4();
	nearest_model_pose_matrix4.SetTranslate( nearest_model_pose.pos );
	cout << nearest_model_pose_matrix4 << endl;

	// calculate error
	math::Matrix4 error_matrix = nearest_model_pose_matrix4.Inverse() * result_world;	// in ground_truth frame
	cout << "Error Euler (degree): " << error_matrix.GetEulerRotation() * 180 / M_PI << endl;
	double error_quaternion_angle;
	math::Vector3 error_quaternion_axis;
	error_matrix.GetRotation().GetAsAxis( error_quaternion_axis, error_quaternion_angle );
	cout << "Error Quaternion Axis : " << error_quaternion_axis << endl;
	cout << "Error Quaternion Angle (degree) : " << error_quaternion_angle * 180 / M_PI << endl;
	cout << "Error Translation : " << error_matrix.GetTranslation() << endl;
	cout << "Error Translation Length: " << error_matrix.GetTranslation().GetLength() << endl;

	// ****************************** //
	// visualize object's pose result //
	// ****************************** //
	int recognized_idx = -1;
	for( uint i = 0; i < m_target_model_names.size(); i++ )
	{
		if( _msg->object_name().compare( 0, m_target_model_names[ i ].size(), m_target_model_names[ i ] ) == 0 )
		{
			// is the result_visualize for estimated object
			_resultVisualize( i, result_world.GetAsPose() );
			recognized_idx = i;
		}
		else	// hide the others
		{
			_resultVisualize( i, math::Pose( -m_stacking_distance * 2 * (i + 1), 1, 2, 0, 0, 0 ) );
		}
	}

	// check if recognized object is identified
	if( recognized_idx < 0 )
	{
		cerr << CERR_PREFIX << " can not identify recognized object!" << endl;
		exit( -1 );
	}

	// ************************************* //
	// check validation of estimation result //
	// ************************************* //
	bool estimate_correct = false;

	// check model recognition
	if( cur_nearest_object->GetName().compare( 0, m_target_model_names[ recognized_idx ].size(), m_target_model_names[ recognized_idx ] ) == 0 )
	{
		const EvaluationCriteria &criteria = m_criteria[ recognized_idx ];

		// check success criteria - translation
		if( translate_error < criteria.translation_threshold )
		{
			// check success criteria - rotation
			if( error_quaternion_angle < criteria.quaternion_degree_threshold * M_PI / 180 )
			{
				estimate_correct = true;
			}
			else
			{
				if( criteria.is_cylinder_like )	// is cylinder like
				{
					if( abs( M_PI - error_quaternion_angle ) < criteria.quaternion_degree_threshold * M_PI / 180 )
					{
						estimate_correct = true;
					}
					else
					{
						float axis_bias_degree = acos( criteria.cylinder_axis.Dot( error_quaternion_axis ) );

						if(	axis_bias_degree < criteria.cylinder_axis_deviation_threshold ||
							180 - axis_bias_degree < criteria.cylinder_axis_deviation_threshold )
						{
							estimate_correct = true;
						}
						else
						{
							cout << CERR_PREFIX << "axis deviation is too large" << endl;
							cout << "axis deviation degree : " << axis_bias_degree << endl;
						}

					}
				}
				else
				{
					// check circular symmetry
					if( criteria.has_circular_symmetry )
					{
						float axis_bias_degree = acos( criteria.cir_sym_axis.Dot( error_quaternion_axis ) ) * 180 / M_PI;
						if(	axis_bias_degree < criteria.cir_sym_axis_deviation_degree ||
							180 - axis_bias_degree < criteria.cir_sym_axis_deviation_degree )
						{
							estimate_correct = true;
						}
						else
						{
							cout << CERR_PREFIX << "Circular symmetry didn't pass" << endl;
							cout << CERR_PREFIX << "axis deviation degree : " << axis_bias_degree << endl;
						}
					}
					// check rotational symmetry
					if( !estimate_correct && criteria.has_rotational_symmetry )
					{
						for( uint i = 0; i < criteria.rot_sym_axes.size(); i++ )
						{
							float axis_bias_degree = acos( criteria.rot_sym_axes[ i ].Dot( error_quaternion_axis ) ) * 180 / M_PI;
							float degree_interval = 360 / criteria.rot_sym_order[ i ];
							float radian_interval = degree_interval * M_PI / 180;

							if(	axis_bias_degree < criteria.rot_sym_axis_deviation_degree[ i ] ||
								180 - axis_bias_degree < criteria.rot_sym_axis_deviation_degree[ i ] )
							{
								for( int j = 0; j < criteria.rot_sym_order[ i ]; j++ )
								{
									if( abs( error_quaternion_angle - radian_interval * j ) < criteria.rot_sym_tolerance_degree[ i ] * M_PI / 180 )
									{
										estimate_correct = true;
										break;
									}
								}

								if( !estimate_correct )
								{
									cout << CERR_PREFIX << "tolerance of rotational symmetry is too big" << endl;
								}
							}
							else
							{
								cout << CERR_PREFIX << "deviation of rotational symmetry axis is too big" << endl;
								cout << CERR_PREFIX << "axis deviation degree : " << axis_bias_degree << endl;
							}
						}
					}

					if( !estimate_correct )	// still not correct
					{
						cout << CERR_PREFIX << "Rotation error is too large" << endl;
					}
				}
			}
		}
		else
		{
			cout << CERR_PREFIX << "Translate_error is too large" << endl;
		}
	}
	else
	{
		cout << CERR_PREFIX << "Wrong model recognized!" << endl;
	}

	// ****************** //
	// estimation logging //
	// ****************** //
	string error_filename = m_log_directory + "error_log";
	string success_filename = m_log_directory + "success_log";

	static int error_log_count = 0;
	static int success_log_count = 0;

	bool do_log = ( estimate_correct && m_success_logging ) || ( !estimate_correct && m_error_logging ) ? true : false;

	if( do_log )
	{
		// save error info
		ofstream file;
		file.open( ( estimate_correct ? success_filename : error_filename ).c_str(), ios::out | ios::app );
		if( file.is_open() )
		{
			file << "[" << ( estimate_correct ? success_log_count : error_log_count ) << "]" << endl;

			// log estimate object
			file << "@Object_Recognized:" << m_target_model_names[ recognized_idx ] << endl;
			// log closest object
			file << "@Closest_Object:" << cur_nearest_object->GetName() << endl;
			// log error info
			file << "@Error Euler (degree):" << error_matrix.GetEulerRotation() * 180 / M_PI << endl;
			file << "@Error Quaternion Axis:" << error_quaternion_axis << endl;
			file << "@Error Quaternion Angle (degree):" << error_quaternion_angle * 180 / M_PI << endl;
			file << "@Error Translation:" << error_matrix.GetTranslation() << endl;
			file << "@Error Translation Length:" << error_matrix.GetTranslation().GetLength() << endl;
			// log estimated pose
			file << "@Estimate_result:" << endl;
			file << result_world;
			// log sensor info
			file << "@Sensor_Pose(not sensor model):" << endl;
			file << m_sensor_pose.GetAsPose() << endl;
			// log every objects
			file << "@Object_Pose:" << endl;
			for( unsigned int i = 0; i < m_models_name.size(); i++ )
			{
				// check if this model have been estimated
				if( m_estimated_models[ i ] )
				{
					continue;
				}

				file << m_models_name[ i ] << ":";
				file << m_world->GetModel( m_models_name[ i ] )->GetWorldPose() << endl;
			}
			// log other object
			unsigned int num_model = m_world->GetModelCount();
			// go through every model in world
			for( unsigned int i = 0; i < num_model; i++ )
			{
				physics::ModelPtr cur_model = m_world->GetModel( i );
				if( cur_model && find( m_models_name.begin(), m_models_name.end(), cur_model->GetName() ) == m_models_name.end() )
				{
					file << cur_model->GetName() << ":";
					file << cur_model->GetWorldPose() << endl;
				}
			}

			file << endl;
			file.close();

			// counter
			if( estimate_correct )
			{
				success_log_count++;
			}
			else
			{
				error_log_count++;
			}
		}
		else
		{
			cout << CERR_PREFIX << "Unable to open file" << endl;
			exit( -1 );
		}
	}

	// ****************************** //
	// save success_between_fail info //
	// ****************************** //
	static int success_before_fail_count = 0;
	// handle inestimable_state
	if( m_inestimable_state )
	{
		m_inestimable_state = false;

		string out_filename = m_log_directory + "success_between_fail_count";
		ofstream file;
		file.open( out_filename.c_str(), ios::out | ios::app );
		if( file.is_open() )
		{
			file << success_before_fail_count << endl;
			file.close();
		}
		else
		{
			cout << CERR_PREFIX << "Unable to open file" << endl;
			exit( -1 );
		}

		// reset counter
		success_before_fail_count = 0;

	}
	// handle normal situation
	if( estimate_correct )
	{
		success_before_fail_count++;
	}
	else
	{
		string out_filename = m_log_directory + "success_between_fail_count";
		ofstream file;
		file.open( out_filename.c_str(), ios::out | ios::app );
		if( file.is_open() )
		{
			file << success_before_fail_count << endl;
			file.close();
		}
		else
		{
			cout << CERR_PREFIX << "Unable to open file" << endl;
			exit( -1 );
		}

		// reset counter
		success_before_fail_count = 0;
	}
	// ************************ //
	// estimation result handle //
	// ************************ //

	// send evaluation result
	gazebo::msgs::Request evaluation_result_request;
	if( estimate_correct )
	{
		evaluation_result_request.set_id( 1 );
	}
	else
	{
		evaluation_result_request.set_id( 0 );
	}
	evaluation_result_request.set_request( "" );

	/*	while( !m_evaluation_result_publisher_ptr->HasConnections() )
	{
		cout << "\033[1;31m" << "evaluation result signal is not connected." << "\033[0m" << endl;
		gazebo::common::Time::MSleep( 10 );
	}*/

	m_evaluation_result_publisher_ptr->Publish( evaluation_result_request );


	if( estimate_correct )
	{
		// hide the correctly estimated object
		m_estimated_models[ cur_nearest_idx ] = true;
		cur_nearest_object->SetWorldPose( math::Pose( m_stacking_distance * 2 * cur_nearest_idx, 1, 2, 0, 0, 0 ) );
	}
	else	// estimation incorrect
	{
		// ********************* //
		// resimulate after_fail //
		// ********************* //
		if( m_resimulate_after_fail )
		{
			// send resimulate request
			msgs::Request resimulate_request;
			resimulate_request.set_id( 0 );
			resimulate_request.set_request( "resimulate" );

			while( !m_resimulate_publisher_ptr->HasConnections() )
			{
				cout << COUT_PREFIX << "\033[1;31m" << "no connection to resimulate request!" << "\033[0m" << endl;
				gazebo::common::Time::MSleep( 10 );
			}
			cout << COUT_PREFIX << "Resimulate request..." << endl;
			m_resimulate_publisher_ptr->Publish( resimulate_request );


			// fake all models are estimated, so the simulation will restart
			m_estimated_models.assign( m_estimated_models.size(), true );
			//this->_receiveEnded( ConstMsgsRequestPtr() );

			// skip this function
			m_skip_receive_result = true;
		}
	}
}

/*
 * Method:    _receiveEnded
 * FullName:  EvaluationPlatform::_receiveEnded
 * Access:    public
 * Qualifier:
 * @param     ConstMsgsRequestPtr &_msg
 *
 * receive ended msg from rbp-console/pose_estimation.cpp
 */
void EvaluationPlatform::_receiveEnded( ConstMsgsRequestPtr &_msg )
{
	cout << COUT_PREFIX << "Estimation process finished." << endl;

	// ********************************************************************** //
	// check whether unestimate count have changed comparing to previous loop //
	// ********************************************************************** //
	static uint prev_unestimate_count = m_stacking_width * m_stacking_height * m_stacking_layers;
	static uint unchange_count = 0;

	uint unestimate_count = 0;

	for( uint i = 0; i < m_models_name.size(); i++ )
	{
		// unestimate
		if( !m_estimated_models[ i ] )
		{
			unestimate_count++;
		}
	}

	unchange_count = unestimate_count != 0 && unestimate_count == prev_unestimate_count ? unchange_count + 1 : 0;

	prev_unestimate_count = unestimate_count;

	// ************************* //
	// handle unchange_count >= 3 //
	// ************************* //
	uint unchange_count_threshold = 3;

	if( unchange_count >= unchange_count_threshold )
	{
		m_inestimable_state = true;

		// reset variables
		prev_unestimate_count = m_stacking_width * m_stacking_height * m_stacking_layers;

		// save current models configurations
		string inestimable = m_log_directory + "inestimable_log";
		static uint inestimable_count = 0;
		ofstream file;
		file.open( inestimable.c_str(), ios::out | ios::app );
		if( file.is_open() )
		{
			file << "[" << inestimable_count << "]" << endl;

			// log estimate object
			file << "@Object_Recognized:" << m_target_model_names[ 0 ] << endl;
			// log closest object
			file << "@Closest_Object:" << "result_visualize_" + m_target_model_names[ 0 ] << endl;
			// log error info
			file << "@Error Euler (degree):" << "0 0 0" << endl;
			file << "@Error Quaternion Axis:" << math::Vector3( 0, 0, 0 ) << endl;
			file << "@Error Quaternion Angle (degree):" << 0 << endl;
			file << "@Error Translation:" << math::Vector3( 0, 0, 0 ) << endl;
			file << "@Error Translation Length:" << 0 << endl;
			// log estimated pose
			file << "@Estimate_result:" << endl;
			file << math::Matrix4( 	1, 0, 0, -1,
									0, 1, 0, 1,
									0, 0, 1, 2,
									0, 0, 0, 1 );
			// log sensor info
			file << "@Sensor_Pose(not sensor model):" << endl;
			file << m_sensor_pose.GetAsPose() << endl;
			// log every objects
			file << "@Object_Pose:" << endl;
			for( unsigned int i = 0; i < m_models_name.size(); i++ )
			{
				// check if this model have been estimated, if true, then skip; if false, then save the model that cannot be estimated
				if( m_estimated_models[ i ] )
				{
					continue;
				}

				file << m_models_name[ i ] << ":";
				file << m_world->GetModel( m_models_name[ i ] )->GetWorldPose() << endl;
			}
			// log other object
			unsigned int num_model = m_world->GetModelCount();
			// go through every model in world, and print the pose of every model in the world coordinate
			for( unsigned int i = 0; i < num_model; i++ )
			{
				physics::ModelPtr cur_model = m_world->GetModel( i );
				if( cur_model && find( m_models_name.begin(), m_models_name.end(), cur_model->GetName() ) == m_models_name.end() )
				{
					file << cur_model->GetName() << ":";
					file << cur_model->GetWorldPose() << endl;
				}
			}

			file << endl;
			file.close();

			inestimable_count++;
		}
		else
		{
			cout << CERR_PREFIX << "Unable to open file" << endl;
			exit( -1 );
		}


		// send evaluation result
		gazebo::msgs::Request evaluation_result_request;
		evaluation_result_request.set_id( 2 );		// inestimable
		evaluation_result_request.set_request( "" );

		m_evaluation_result_publisher_ptr->Publish( evaluation_result_request );
	}

	// ******** //
	//
	// ******** //

	// if all objects have been estimated, reset all models
	if( unestimate_count == 0 || unchange_count >= unchange_count_threshold )
	{
		for( unsigned int i = 0; i < m_models_name.size(); i++ )
		{
			physics::ModelPtr cur_model = m_world->GetModel( m_models_name[ i ] );

			cur_model->SetEnabled( true );
			cur_model->SetStatic( false );
			cur_model->SetGravityMode( true );

			vector< physics::LinkPtr > links = cur_model->GetLinks();
			for( unsigned int j = 0; j < links.size(); j++ )
			{
				links[ j ]->SetKinematic( false );						// this is important
			}
		}
		// reset m_estimated_models
		m_estimated_models.assign( m_models_name.size(), false );

		// re-throw objects
		this->_throwObjects();

		// set m_rethrowed flag
		m_rethrowed = true;

		// reset variables
		if( unchange_count >= 5 )
		{
			unchange_count = 0;
		}
	}
	else
	{
		// reactivate the objects staking simulation
		for( unsigned int i = 0; i < m_models_name.size(); i++ )
		{
			// check if this model have been estimated
			if( m_estimated_models[ i ] )
			{
				continue;
			}

			physics::ModelPtr cur_model = m_world->GetModel( m_models_name[ i ] );
			cur_model->SetEnabled( true );
			cur_model->SetStatic( false );
			cur_model->SetGravityMode( true );

			vector< physics::LinkPtr > links = cur_model->GetLinks();
			for( unsigned int j = 0; j < links.size(); j++ )
			{
				links[ j ]->SetKinematic( false );						// this is important
			}
		}
	}

	// hide all result_visualize
	for( uint i = 0; i < m_target_model_names.size(); i++ )
	{
		_resultVisualize( i, math::Pose( -m_stacking_distance * 2 * (i + 1), 1, 2, 0, 0, 0 ) );
	}

	// *************************************** //
	// reconnect the world update begin events //
	// *************************************** //
	this->m_update_connection = event::Events::ConnectWorldUpdateBegin( boost::bind(&EvaluationPlatform::_onUpdate, this, _1 ) );
}

//** Constructing the blue box **//
void EvaluationPlatform::_environmentConstruction()
{
	// starting position of the box
	m_box_center = math::Vector3( 0, 0, 0 );
	m_stacking_center = math::Vector3( 0, 0, m_box_size.z + m_box_wall_thickness + m_throwing_height ) + math::Vector3( m_box_center.x, m_box_center.y, 0 );

	// ************* //
	// construct bin //
	// ************* //
	sdf::SDF box_sdf;

	// read ascii file into string
	string filename = m_box_model_sdf_file_path;
	std::string contents;
	// method from http://stackoverflow.com/questions/2602013/read-whole-ascii-file-into-c-stdstring
	std::ifstream box_in( filename.c_str(), std::ios::in );
	if( box_in )
	{
		box_in.seekg(0, std::ios::end);
		contents.resize(box_in.tellg());
		box_in.seekg(0, std::ios::beg);
		box_in.read(&contents[0], contents.size());
		box_in.close();
	}
	// set box sdf from string
	box_sdf.SetFromString( contents );

	// calculate box parameters
	float wall_thickness = m_box_wall_thickness;

	math::Vector3 bottom_size( m_box_size.x, m_box_size.y, wall_thickness );
	math::Pose bottom_pose( 0, 0, wall_thickness / 2, 0, 0, 0 );

	math::Vector3 front_size( m_box_size.x + 2 * wall_thickness, wall_thickness, m_box_size.z + wall_thickness );
	math::Pose front_pose( 0, m_box_size.y / 2 + wall_thickness / 2, front_size.z / 2, 0, 0, 0 );

	math::Vector3 back_size( m_box_size.x + 2 * wall_thickness, wall_thickness, m_box_size.z + wall_thickness );
	math::Pose back_pose( 0, -(m_box_size.y / 2 + wall_thickness / 2), back_size.z / 2, 0, 0, 0 );

	math::Vector3 left_size( wall_thickness, m_box_size.y, m_box_size.z + wall_thickness );
	math::Pose left_pose( -(m_box_size.x / 2 + wall_thickness / 2), 0, left_size.z / 2, 0, 0, 0 );

	math::Vector3 right_size( wall_thickness, m_box_size.y, m_box_size.z + wall_thickness );
	math::Pose right_pose( m_box_size.x / 2 + wall_thickness / 2, 0, right_size.z / 2, 0, 0, 0 );

	// resize the box as specified in parameters file
	sdf::ElementPtr box_link = box_sdf.root->GetElement( "model" )->GetElement( "link" );
	// set bottom box
	sdf::ElementPtr box_collision = box_link->GetElement( "collision" );
	sdf::ElementPtr box_visual = box_link->GetElement( "visual" );
	box_collision->GetElement( "pose" )->Set( bottom_pose );
	box_collision->GetElement( "geometry" )->GetElement( "box" )->GetElement( "size" )->Set( bottom_size );
	box_visual->GetElement( "pose" )->Set( bottom_pose );
	box_visual->GetElement( "geometry" )->GetElement( "box" )->GetElement( "size" )->Set( bottom_size );
	// set front box
	box_collision = box_collision->GetNextElement( "collision" );
	box_visual = box_visual->GetNextElement( "visual" );
	box_collision->GetElement( "pose" )->Set( front_pose );
	box_collision->GetElement( "geometry" )->GetElement( "box" )->GetElement( "size" )->Set( front_size );
	box_visual->GetElement( "pose" )->Set( front_pose );
	box_visual->GetElement( "geometry" )->GetElement( "box" )->GetElement( "size" )->Set( front_size );
	// set back box
	box_collision = box_collision->GetNextElement( "collision" );
	box_visual = box_visual->GetNextElement( "visual" );
	box_collision->GetElement( "pose" )->Set( back_pose );
	box_collision->GetElement( "geometry" )->GetElement( "box" )->GetElement( "size" )->Set( back_size );
	box_visual->GetElement( "pose" )->Set( back_pose );
	box_visual->GetElement( "geometry" )->GetElement( "box" )->GetElement( "size" )->Set( back_size );
	// set left box
	box_collision = box_collision->GetNextElement( "collision" );
	box_visual = box_visual->GetNextElement( "visual" );
	box_collision->GetElement( "pose" )->Set( left_pose );
	box_collision->GetElement( "geometry" )->GetElement( "box" )->GetElement( "size" )->Set( left_size );
	box_visual->GetElement( "pose" )->Set( left_pose );
	box_visual->GetElement( "geometry" )->GetElement( "box" )->GetElement( "size" )->Set( left_size );
	// set right box
	box_collision = box_collision->GetNextElement( "collision" );
	box_visual = box_visual->GetNextElement( "visual" );
	box_collision->GetElement( "pose" )->Set( right_pose );
	box_collision->GetElement( "geometry" )->GetElement( "box" )->GetElement( "size" )->Set( right_size );
	box_visual->GetElement( "pose" )->Set( right_pose );
	box_visual->GetElement( "geometry" )->GetElement( "box" )->GetElement( "size" )->Set( right_size );

	// insert box model sdf to world
	m_world->InsertModelSDF( box_sdf );

	// ************************************ //
	// read multiple target model sdf files //
	// ************************************ //

	// store multiple target model sdfs //
	vector< sdf::SDF > model_sdfs;

	vector< sdf::ElementPtr > sdf_model_elements;
	vector< sdf::ParamPtr > sdf_model_names;

	vector< sdf::ParamPtr > sdf_model_poses;

	for( uint i = 0; i < m_target_model_sdf_file_paths.size(); i++ )
	{
		sdf::SDF cur_sdf;

		// read ascii file into string
		filename = m_target_model_sdf_file_paths[ i ];
		// method from http://stackoverflow.com/questions/2602013/read-whole-ascii-file-into-c-stdstring
		std::ifstream in( filename.c_str(), std::ios::in );
		if( in )
		{
			in.seekg(0, std::ios::end);
			contents.resize(in.tellg());
			in.seekg(0, std::ios::beg);
			in.read(&contents[0], contents.size());
			in.close();
		}
		// set model sdf from string
		cur_sdf.SetFromString( contents );

		// store current target model sdf
		model_sdfs.push_back( cur_sdf );

		// do not use cur_sdf afterwards

		// get template SDF's model name pointer
		sdf::ElementPtr model_element = model_sdfs.back().root->GetElement( "model" );
		sdf::ParamPtr model_name;
		if( model_element->HasAttribute( "name" ) )
		{
			model_name = model_element->GetAttribute( "name" );
			m_target_model_names.push_back( model_name->GetAsString() );

			sdf_model_elements.push_back( model_element );
			sdf_model_names.push_back( model_name );
		}
		else
		{
			gzerr << "weird result occured ( element[model] doesn't have attribute[name]" << endl;
			return;
		}

		sdf::ElementPtr pose_element;
		sdf::ParamPtr model_pose;

		if( model_element->HasElement( "pose" ) )
		{
			pose_element = model_element->GetElement( "pose" );
			model_pose = pose_element->GetValue();
			std::cout << "model_pose : " << *model_pose << std::endl;
			sdf_model_poses.push_back( model_pose );
		}
		else
		{
			gzerr << "no model pose in model file" << endl;
			return;
		}

	}

	// ************************** //
	// create a handful of object //
	// ************************** //

	// store each model count
	vector< int > each_model_counts( m_target_model_names.size(), 0 );

	// create random pose
	int width = m_stacking_width;
	int height = m_stacking_height;
	int layers = m_stacking_layers;

	for( int j = 0; j < layers ; j++ )
	{
		for( int i = 0; i < width * height; i++ )
		{
			// current object's position
			math::Vector3 position = m_stacking_center + j * math::Vector3( 0, 0, m_stacking_distance );
			// x position
			if( width % 2 == 0 )	// even number
			{
				position += ( ( i % width ) - width / 2 + 0.5 ) * math::Vector3( m_stacking_distance, 0, 0 );
			}
			else	// odd number
			{
				position += ( ( i % width ) - width / 2 ) * math::Vector3( m_stacking_distance, 0, 0 );
			}

			// y position
			if( height % 2 == 0 )	// even number
			{
				position += ( ( i / width ) - height / 2 + 0.5 ) * math::Vector3( 0, m_stacking_distance, 0 );
			}
			else	// odd number
			{
				position += ( ( i / width ) - height / 2 ) * math::Vector3( 0, m_stacking_distance, 0 );
			}

			// randomize the pose
			int angleIndex = math::Rand::GetIntUniform( 0, 7 );			//0~7   0~359   0=0 ; 1=45 ... 7=315
			int axisIndex = math::Rand::GetIntUniform( 0, 1 );         //0 = x-axis  ; 1 = y-axis
			math::Vector3 rotate_axis;
			if( axisIndex == 1 )
				rotate_axis = math::Vector3( 0, 1, 0 );
			else
				rotate_axis = math::Vector3( 1, 0, 0 );

			math::Angle angle;
			angle.SetFromDegree( 45 * angleIndex );

			math::Quaternion orientation = math::Quaternion( rotate_axis, angle.Degree() );
			math::Vector3 euler = orientation.GetAsEuler();

			// randomly choose one of the target models according to the proportion
			int total_proportion = std::accumulate( m_target_model_proportions.begin(), m_target_model_proportions.end(), 0 );
			int cur_idx = math::Rand::GetIntUniform( 0, total_proportion - 1 );
			int cur_proportion = 0;
			int chose_model_idx = -1;
			for( uint idx = 0; idx < m_target_model_proportions.size(); idx++ )
			{
				cur_proportion += m_target_model_proportions[ idx ];
				if( cur_idx <= cur_proportion - 1 )
				{
					chose_model_idx = idx;
					break;
				}
			}

			if( chose_model_idx < 0 || chose_model_idx >= (int)m_target_model_names.size() )
			{
				cerr << CERR_PREFIX << "error when choosing random target model" << endl;
				exit( -1 );
			}

			// create models from SDF template
			stringstream ss;
			ss << each_model_counts[ chose_model_idx ];

			each_model_counts[ chose_model_idx ]++;

			string cur_model_name = m_target_model_names[ chose_model_idx ] + "_" + ss.str();

			sdf_model_names[ chose_model_idx ]->Set( cur_model_name );
			sdf_model_poses[ chose_model_idx ]->Set( math::Pose( position.x, position.y, position.z, euler.x, euler.y, euler.z ) );

			m_world->InsertModelSDF( model_sdfs[ chose_model_idx ] );

			m_models_name.push_back( cur_model_name );
		}
	}

	// initialize some variables
	m_estimated_models.assign( m_models_name.size(), false );

	// *********************************************** //
	// create result visualizer for every target model //
	// *********************************************** //

	for( uint i = 0; i < m_target_model_names.size(); i++ )
	{
		// remove physics ( inertial, collision, etc.
		sdf::ElementPtr link_element = sdf_model_elements[ i ]->GetElement( "link" );
		if( link_element->HasElement( "inertial" ) )
		{
			link_element->RemoveChild( link_element->GetElement( "inertial" ) );
		}
		if( link_element->HasElement( "collision" ) )
		{
			link_element->RemoveChild( link_element->GetElement( "collision" ) );
		}
		// set object to kinematics
		if( !link_element->HasElement( "gravity" ) )
		{
			link_element->AddElement( "gravity" );
		}
		link_element->GetElement( "gravity" )->GetValue()->Set( false );
		// set object to kinematics
		if( !link_element->HasElement( "kinematic" ) )
		{
			link_element->AddElement( "kinematic" );
		}
		link_element->GetElement( "kinematic" )->GetValue()->Set( true );

		// set material
		sdf::ElementPtr visual = link_element->GetElement( "visual" );

		// handle multiple visuals
		while( visual )
		{
			// TODO : according to sdf API, should remove <script> element before color elements can be effective
			if( !visual->HasElement( "material" ) )
			{
				visual->AddElement( "material" );
			}
			sdf::ElementPtr material = visual->GetElement( "material" );
			if( !material->HasElement( "ambient" ) )
			{
				material->AddElement( "ambient" );
			}
			if( !material->HasElement( "diffuse" ) )
			{
				material->AddElement( "diffuse" );
			}
			if( !material->HasElement( "specular" ) )
			{
				material->AddElement( "specular" );
			}
			material->GetElement( "ambient" )->GetValue()->Set( sdf::Color( 0, 1, 0, 1 ) );
			material->GetElement( "diffuse" )->GetValue()->Set( sdf::Color( 0, 1, 0, 1 ) );
			material->GetElement( "specular" )->GetValue()->Set( sdf::Color( 0, 0.5, 0, 1 ) );

			// get next visual element
			visual = visual->GetNextElement( "visual" );
		}

		// insert result visualize
		sdf_model_names[ i ]->Set( "result_visualize_" + m_target_model_names[ i ] );
		sdf_model_poses[ i ]->Set( math::Pose( -m_stacking_distance * 2 * (i + 1), 1, 2, 0, 0, 0 ) );
		m_world->InsertModelSDF( model_sdfs[ i ] );

	}

	cout << COUT_PREFIX << "creation complete" << endl;
}

void EvaluationPlatform::_rethrowForOnlySnapshot( ConstMsgsRequestPtr &_msgs )
{
	// if the number of snapshot reach the desired number
	if(stoi(_msgs->data()) == m_total_snapshot)
	{
		m_current_snapshot = stoi(_msgs->data());
		return;
	}

	for( unsigned int i = 0; i < m_models_name.size(); i++ )
	{
		physics::ModelPtr cur_model = m_world->GetModel( m_models_name[ i ] );

		cur_model->SetEnabled( true );
		cur_model->SetStatic( false );
		cur_model->SetGravityMode( true );

		vector< physics::LinkPtr > links = cur_model->GetLinks();
		for( unsigned int j = 0; j < links.size(); j++ )
		{
			links[ j ]->SetKinematic( false );						// this is important
		}
	}
	// reset m_estimated_models
	m_estimated_models.assign( m_models_name.size(), false );

	// re-throw objects
	this->_throwObjects();

	// set m_rethrowed flag
	m_rethrowed = true;

	// *************************************** //
	// reconnect the world update begin events //
	// *************************************** //
	this->m_update_connection = event::Events::ConnectWorldUpdateBegin( boost::bind(&EvaluationPlatform::_onUpdate, this, _1 ) );
}

void EvaluationPlatform::_throwObjects()
{
	// create random pose
	int width = m_stacking_width;
	int height = m_stacking_height;
	int layers = m_stacking_layers;

	for( int j = 0; j < layers ; j++ )
	{
		for( int i = 0; i < width * height; i++ )
		{
			// current object's position
			math::Vector3 position = m_stacking_center + j * math::Vector3( 0, 0, m_stacking_distance );
			// x position
			if( width % 2 == 0 )	// even number
			{
				position += ( ( i % width ) - width / 2 + 0.5 ) * math::Vector3( m_stacking_distance, 0, 0 );
			}
			else	// odd number
			{
				position += ( ( i % width ) - width / 2 ) * math::Vector3( m_stacking_distance, 0, 0 );
			}

			// y position
			if( height % 2 == 0 )	// even number
			{
				position += ( ( i / width ) - height / 2 + 0.5 ) * math::Vector3( 0, m_stacking_distance, 0 );
			}
			else	// odd number
			{
				position += ( ( i / width ) - height / 2 ) * math::Vector3( 0, m_stacking_distance, 0 );
			}

			// randomize the pose
			int angleIndex = math::Rand::GetIntUniform( 0, 7 );		//0~7   0~359   0=0 ; 1=45 ... 7=315
			int axisIndex = math::Rand::GetIntUniform( 0, 1 );		//0 = x-axis  ; 1 = y-axis
			math::Vector3 rotate_axis;
			if( axisIndex == 1 )
				rotate_axis = math::Vector3( 0, 1, 0 );
			else
				rotate_axis = math::Vector3( 1, 0, 0 );

			math::Angle angle;
			angle.SetFromDegree( 45 * angleIndex );

			math::Quaternion orientation = math::Quaternion( rotate_axis, angle.Degree() );
			math::Vector3 euler = orientation.GetAsEuler();

			// set pose
			physics::ModelPtr cur_model = m_world->GetModel( m_models_name[ i + width * height * j ] );

			cur_model->SetWorldPose( math::Pose( position.x, position.y, position.z, euler.x, euler.y, euler.z ) );
		}
	}
}

void EvaluationPlatform::_resultVisualize( const int _model_idx, const math::Pose _pose )
{
	physics::ModelPtr visual_model = m_world->GetModel( "result_visualize_" + m_target_model_names[ _model_idx ] );
	if( visual_model )
	{
		visual_model->SetWorldPose( _pose );
	}
	else
	{
		cout << CERR_PREFIX << "Model : result_visualize not ready yet" << endl;
	}
}


void EvaluationPlatform::_initParameters( const std::string &_filename )
{
	// ************************* //
	// read parameters from file //
	// ************************* //
	try
    {
        using namespace boost::property_tree;

        // read parameters file
        ptree pt;
        read_xml( _filename, pt );

        // read attributes parameters
        m_resimulate_after_fail = pt.get< bool >( "evaluation_platform.attribute.resimulate_after_fail", false );

        // ************************ //
        // read snapshot parameters //
        // ************************ //
        m_snapshot_mode = pt.get< int >( "evaluation_platform.snapshot.snapshot_mode", 0 );
        //std::cout << "m_snapshot_mode : " << m_snapshot_mode << std::endl;
        m_total_snapshot = pt.get< int >( "evaluation_platform.snapshot.total_snapshot", 0 );
        //std::cout << "m_total_snapshot : "<< m_total_snapshot << std::endl;

        // ************************ //
        // read stacking parameters //
        // ************************ //
        m_box_model_sdf_file_path = pt.get< string >( "evaluation_platform.stacking.box_model_sdf_file_path", string() );
        _optimizePathFromXML( m_box_model_sdf_file_path );
        m_box_size = pt.get< math::Vector3 >( "evaluation_platform.stacking.box_size", math::Vector3( 0.21, 0.16, 0.08 ) );
        m_box_wall_thickness = pt.get< float >( "evaluation_platform.stacking.box_wall_thickness", 0.02f );

        // load target models parameters
        ptree child_pt = pt.get_child( "evaluation_platform.stacking" );
        string template_path = "target_model_";

        for( int i = 0; true; i++ )
        {
        	stringstream ss;
        	ss << i;
        	string key_name = template_path + ss.str();
        	if( child_pt.find( key_name ) == child_pt.not_found() )
        	{
        		break;
        	}

        	ptree cur_pt = child_pt.get_child( key_name );

        	int cur_proportion = cur_pt.get< int >( "proportion", 0 );

        	if( cur_proportion > 0 )
        	{
        		string sdf_file_path = cur_pt.get< string >( "sdf_file_path", string() );
        		_optimizePathFromXML( sdf_file_path );

				m_target_model_sdf_file_paths.push_back( sdf_file_path );
				m_target_model_proportions.push_back( cur_proportion );

				// extract evaluation criteria
				EvaluationCriteria criteria;

				criteria.translation_threshold = cur_pt.get< float >( "translation_threshold", 0.0025 );
				criteria.quaternion_degree_threshold = cur_pt.get< float >( "quaternion_degree_threshold", 10 );

				// rotational symmetry
				criteria.has_rotational_symmetry = cur_pt.get< bool >( "rotational_symmetry.<xmlattr>.enable", false );

				if( criteria.has_rotational_symmetry )
				{
					ptree rot_pt = cur_pt.get_child( "rotational_symmetry" );
					// extract rotational symmetry axes
					for( int idx = 0; true; idx++ )
					{
						stringstream ss;
						ss << idx;
						string axis_name = "axis_" + ss.str();
						if( rot_pt.find( axis_name ) == rot_pt.not_found() )
						{
							break;
						}

						ptree cur_pt = rot_pt.get_child( axis_name );

						int order = cur_pt.get< int >( "order", -987654 );
						float tolerance_degree = cur_pt.get< float >( "tolerance_degree", -987654 );
						float axis_deviation_degree = cur_pt.get< float >( "axis_deviation_threshold", -987654 );
						// check validation of rotational symmetry parameters
						if(	order == -987654||
							tolerance_degree == -987654 ||
							axis_deviation_degree == -987654 )
						{
							cerr << CERR_PREFIX << "problem in (" << key_name << "," << axis_name << ")" << endl;
							continue;
						}
						if( order < 2 )
						{
							cerr << CERR_PREFIX << "order of rotational symmetry is not valid (" << key_name << "," << axis_name << ")" << endl;
							cerr << "\t" << "should be bigger than 2 " << endl;
							continue;
						}
						if( tolerance_degree < 0 || axis_deviation_degree < 0 )
						{
							cerr << CERR_PREFIX << "tolerance_degree and axis deviation should be bigger than 0 (" << key_name << "," << axis_name << ")" << endl;
							continue;
						}
						// save cur rotational symmetric axis
						math::Vector3 cur_axis = cur_pt.get< math::Vector3 >( "<xmlattr>.axis", math::Vector3( 0, 0, 0 ) );
						if( cur_axis == math::Vector3( 0, 0, 0 ) )
						{
							cerr << CERR_PREFIX << "problem with the rotational axis (" << key_name << "," << axis_name << ")" << endl;
							continue;
						}
						cur_axis = cur_axis.Normalize();
						criteria.rot_sym_axes.push_back( cur_axis );
						criteria.rot_sym_order.push_back( order );
						criteria.rot_sym_tolerance_degree.push_back( tolerance_degree );
						criteria.rot_sym_axis_deviation_degree.push_back( axis_deviation_degree );
					}

					// check if any axis is extracted, if not, set "has_rotational_symmetry" to false
					if( criteria.rot_sym_axes.size() == 0 )
					{
						criteria.has_rotational_symmetry = false;
					}
				}

				// circular symmetry
				criteria.has_circular_symmetry = cur_pt.get< bool >( "circular_symmetry.<xmlattr>.enable", false );
				if( criteria.has_circular_symmetry )
				{
					ptree cir_pt = cur_pt.get_child( "circular_symmetry" );
					// extract circular symmetry axis   ( should only be one axis
					criteria.cir_sym_axis = cir_pt.get< math::Vector3 >( "axis", math::Vector3( 0, 0, 0 ) );
					criteria.cir_sym_axis_deviation_degree = cir_pt.get< float >( "axis_deviation_threshold", -987654 );
					if( criteria.cir_sym_axis == math::Vector3( 0, 0, 0 ) || criteria.cir_sym_axis_deviation_degree < 0 )
					{
						cerr << CERR_PREFIX << "problem with the circular symmetry (" << key_name << ")" << endl;
						criteria.has_circular_symmetry = false;
					}
					criteria.cir_sym_axis = criteria.cir_sym_axis.Normalize();

				}

				// cylinder like evaluation criteria
				criteria.is_cylinder_like = cur_pt.get< bool >( "cylinder_like.<xmlattr>.enable", false );
				if( criteria.is_cylinder_like )
				{
					ptree cyl_pt = cur_pt.get_child( "cylinder_like" );
					// extract circular symmetry axis   ( should only be one axis
					criteria.cylinder_axis = cyl_pt.get< math::Vector3 >( "cylinder_axis", math::Vector3( 0, 0, 0 ) );
					criteria.cylinder_axis_deviation_threshold = cyl_pt.get< float >( "axis_deviation_threshold", -987654 );
					if( criteria.cylinder_axis == math::Vector3( 0, 0, 0 ) || criteria.cylinder_axis_deviation_threshold < 0 )
					{
						cerr << CERR_PREFIX << "problem with the cylinder_like (" << key_name << ")" << endl;
						criteria.is_cylinder_like = false;
					}
					criteria.cylinder_axis = criteria.cylinder_axis.Normalize();
				}

				// push back criteria
				m_criteria.push_back( criteria );
        	}
        }

        for( uint i = 0; i < m_target_model_proportions.size(); i++ )
        {
        	cout << "target model " << i << " :" << endl;
        	cout << "\t" << "sdf_file_path : " << m_target_model_sdf_file_paths[ i ] << endl;
        	cout << "\t" << "proportion : " << m_target_model_proportions[ i ] << endl;

        	cout << m_criteria[ i ] << endl;
        }

        // steady parameters
        m_check_steady_interval = pt.get< double >( "evaluation_platform.stacking.check_steady_interval", 0.1 );
        m_consecutive_steady_threshold = pt.get< int >( "evaluation_platform.stacking.consecutive_steady_threshold", 5 );
        m_linear_vel_threshold = pt.get< double >( "evaluation_platform.stacking.linear_vel_threshold", 0.03 );

        // stacking parameters
        m_stacking_width = pt.get< int >( "evaluation_platform.stacking.width", 3 );
        m_stacking_height = pt.get< int >( "evaluation_platform.stacking.height", 3 );
        m_stacking_layers = pt.get< int >( "evaluation_platform.stacking.layers", 1 );
        m_stacking_distance = pt.get< float >( "evaluation_platform.stacking.distance_between_objects", 0.07 );
        m_throwing_height = pt.get< float >( "evaluation_platform.stacking.throwing_height", 0.15 );

        // read log parameters
        m_log_directory = pt.get< string >( "evaluation_platform.log.path", "evaluation_log" );
        m_error_logging = pt.get< bool >( "evaluation_platform.log.error_logging", true );
        m_success_logging = pt.get< bool >( "evaluation_platform.log.success_logging", false );
    }
    catch (...)
    {
        std::cerr << "[ERROR] Cannot load the parameter file!" << std::endl;
        exit( -1 );
    }

}

void EvaluationPlatform::_initLog( const std::string &_param_filename )
{
    // ****************************** //
	// create directories for logging //
    // ****************************** //
    _optimizePathFromXML( m_log_directory );

    if( *( m_log_directory.end() - 1 ) != '/' )	// check whether last char is '/'
    {
    	// if not, add '/' to the end
    	m_log_directory.push_back( '/' );
    }
	boost::filesystem::create_directory( m_log_directory );

	stringstream ss;
	ss << gazebo::math::Rand::GetSeed();

	// time
	m_log_directory += boost::posix_time::to_iso_string( boost::posix_time::second_clock::local_time() ) + "_";
	// seed
	m_log_directory += ss.str();

	// add model name in directory
	for( uint i = 0; i < m_target_model_names.size(); i++ )
	{
		if( m_target_model_proportions[ i ] > 0 )
		{
			m_log_directory += "_" + m_target_model_names[ i ];
		}
	}
	// add total objects in direcotry path
	ss.clear();
	ss.str( "" );
	ss << m_stacking_width * m_stacking_height * m_stacking_layers;
	m_log_directory += "_" + ss.str();

	m_log_directory += "/";

	boost::filesystem::create_directory( m_log_directory );

	// ************************************* //
	// copy parameters file to log directory //
	// ************************************* //
	//boost::filesystem::copy_file( _param_filename, m_log_directory + _param_filename );

	std::cout << "log file has been written to : " << m_log_directory << std::endl;
}

void EvaluationPlatform::_optimizePathFromXML( string &_path )
{
    while( *_path.begin() == ' ' ) // check if there is ' ' in the beginning
    {
    	_path.erase( _path.begin() );
    }
    while( *( _path.end() - 1 ) == ' ' ) // check if there is ' ' in the end
    {
    	_path.erase( _path.end() - 1 );
    }
}
