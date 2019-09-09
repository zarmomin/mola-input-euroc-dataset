/* -------------------------------------------------------------------------
 *   A Modular Optimization framework for Localization and mApping  (MOLA)
 * Copyright (C) 2018-2019 Jose Luis Blanco, University of Almeria
 * See LICENSE for license information.
 * ------------------------------------------------------------------------- */
/**
 * @file   EurocDataset.cpp
 * @brief  RawDataSource from Euroc odometry/SLAM datasets
 * @author Jose Luis Blanco Claraco
 * @date   Jan 11, 2019
 */

/** \defgroup mola_input_euroc_dataset_grp mola_input_euroc_dataset_grp.
 * RawDataSource from Euroc odometry/SLAM datasets.
 *
 *
 */

#include <mola-input-euroc-dataset/EurocDataset.h>
#include <mola-kernel/variant_helper.h>
#include <mola-kernel/yaml_helpers.h>
#include <mrpt/core/initializer.h>
#include <mrpt/obs/CObservationIMU.h>
#include <mrpt/obs/CObservationImage.h>
#include <mrpt/system/filesystem.h>  //ASSERT_DIRECTORY_EXISTS_()
#include <yaml-cpp/yaml.h>
#include <Eigen/Dense>
#include <fstream>
// Eigen must be before csv.h
#include <mrpt/io/csv.h>

using namespace mola;

// arguments: class_name, parent_class, class namespace
IMPLEMENTS_MRPT_OBJECT_NS_PREFIX(EurocDataset, RawDataSourceBase, mola);

MRPT_INITIALIZER(do_register_EurocDataset)
{
    MOLA_REGISTER_MODULE(EurocDataset);
}

EurocDataset::EurocDataset() = default;

void EurocDataset::initialize(const std::string& cfg_block)
{
    using namespace std::string_literals;

    MRPT_START
    ProfilerEntry tle(profiler_, "initialize");

    // Mandatory parameters:
    auto c = YAML::Load(cfg_block);

    ensureYamlEntryExists(c, "params");
    auto cfg = c["params"];
    MRPT_LOG_DEBUG_STREAM("Initializing with these params:\n" << cfg);

    yamlLoadMemberReq<std::string>(cfg, "base_dir", &base_dir_);
    yamlLoadMemberReq<std::string>(cfg, "sequence", &sequence_);

    seq_dir_ = base_dir_ + "/"s + sequence_ + "/mav0"s;
    ASSERT_DIRECTORY_EXISTS_(seq_dir_);
    ASSERT_DIRECTORY_EXISTS_(seq_dir_ + "/cam0"s);
    ASSERT_DIRECTORY_EXISTS_(seq_dir_ + "/cam1"s);
    ASSERT_DIRECTORY_EXISTS_(seq_dir_ + "/imu0"s);

    // Optional params with default values:
    yamlLoadMemberOpt<double>(cfg, "time_warp_scale", &time_warp_scale_);

    // Preload everything we may need later to quickly replay the dataset in
    // realtime:
    MRPT_LOG_INFO_STREAM("Loading EUROC dataset from: " << seq_dir_);

    // Cameras: cam{0,1}
    for (uint8_t cam_id = 0; cam_id < 2; cam_id++)
    {
        Eigen::Matrix<uint64_t, Eigen::Dynamic, Eigen::Dynamic> dat;

        const auto cam_data_fil =
            seq_dir_ + "/cam"s + std::to_string(cam_id) + "/data.csv"s;
        ASSERT_FILE_EXISTS_(cam_data_fil);

        mrpt::io::load_csv(cam_data_fil, dat);
        ASSERT_(dat.cols() == 2);
        ASSERT_(dat.rows() > 10);

        SensorCamera se_cam;
        se_cam.sensor_name = "cam"s + std::to_string(cam_id);
        se_cam.cam_idx     = cam_id;

        for (int row = 0; row < dat.rows(); row++)
        {
            const auto t = static_cast<euroc_timestamp_t>(dat(row, 0));
            se_cam.img_file_name =
                "/cam"s + std::to_string(cam_id) + "/data/"s +
                std::to_string(static_cast<euroc_timestamp_t>(dat(row, 1))) +
                ".png"s;

            dataset_.emplace_hint(dataset_.end(), t, se_cam);
        }

        MRPT_LOG_INFO_STREAM(
            "cam" << std::to_string(cam_id) << ": Loaded " << dat.rows()
                  << " entries.");

        // Load calibration:
        const auto fil_calib =
            seq_dir_ + "/cam"s + std::to_string(cam_id) + "/sensor.yaml"s;
        ASSERT_FILE_EXISTS_(fil_calib);
        auto cal = YAML::LoadFile(fil_calib);

        // Camera pose:
        ensureYamlEntryExists(cal, "T_BS");
        auto T_BS = cal["T_BS"];
        ensureYamlEntryExists(T_BS, "data");
        auto cam_pose = T_BS["data"];

        const mrpt::poses::CPose3D imu2veh(
            0, 0, 0, mrpt::DEG2RAD(180.0), mrpt::DEG2RAD(-90.0),
            mrpt::DEG2RAD(0.0));

        mrpt::math::CMatrixDouble44 HM;

        auto itN = cam_pose.begin();
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++, ++itN) HM(r, c) = itN->as<double>();

        // EUROC uses as body frame the IMU coordinates,
        // with +X pointing UP, +Y pointing left.
        // Let's transform cameras to match our uniform convention for cameras:
        // +X & +Y in the right/down image plane, as seen from a vehicle frame
        // with +Z up, +X forward.
        cam_poses_[cam_id] = (imu2veh + mrpt::poses::CPose3D(HM)).asTPose();

        // Camera intrinsics:
        auto intrinsics = cal["intrinsics"];
        auto itI        = intrinsics.begin();

        const double fx = (itI++)->as<double>();
        const double fy = (itI++)->as<double>();
        const double cx = (itI++)->as<double>();
        const double cy = (itI++)->as<double>();
        cam_intrinsics_[cam_id].setIntrinsicParamsFromValues(fx, fy, cx, cy);

        // Camera distortion model:
        ASSERT_(
            cal["distortion_model"].as<std::string>() == "radial-tangential");

        auto dists = cal["distortion_coefficients"];
        auto itD   = dists.begin();
        // k1 k2 p1 p1
        const double k1 = (itD++)->as<double>();
        const double k2 = (itD++)->as<double>();
        const double p1 = (itD++)->as<double>();
        const double p2 = (itD++)->as<double>();

        cam_intrinsics_[cam_id].dist[0] = k1;
        cam_intrinsics_[cam_id].dist[1] = k2;
        cam_intrinsics_[cam_id].dist[2] = p1;
        cam_intrinsics_[cam_id].dist[3] = p2;
        cam_intrinsics_[cam_id].dist[4] = 0;  // k3

    }  // end for each camera

    // IMU:
    {
        Eigen::MatrixXd dat;

        const auto imu_data_fil = seq_dir_ + "/imu0/data.csv"s;
        ASSERT_FILE_EXISTS_(imu_data_fil);

        mrpt::io::load_csv(imu_data_fil, dat);

        // Example rows:
        // clang-format off
        // #timestamp [ns],w_RS_S_x [rad s^-1],w_RS_S_y [rad s^-1],w_RS_S_z [rad s^-1],a_RS_S_x [m s^-2],a_RS_S_y [m s^-2],a_RS_S_z [m s^-2]
        // 1403636579758555392,-0.099134701513277898,0.14730578886832138,0.02722713633111154,8.1476917083333333,-0.37592158333333331,-2.4026292499999999
        // clang-format on
        ASSERT_(dat.cols() == 7);
        ASSERT_(dat.rows() > 10);

        SensorIMU se_imu;
        se_imu.sensor_name = "imu0";

        for (int row = 0; row < dat.rows(); row++)
        {
            const auto t = static_cast<euroc_timestamp_t>(dat(row, 0));
            se_imu.wx    = dat(row, 1);
            se_imu.wy    = dat(row, 2);
            se_imu.wz    = dat(row, 3);
            se_imu.accx  = dat(row, 4);
            se_imu.accy  = dat(row, 5);
            se_imu.accz  = dat(row, 6);

            dataset_.emplace_hint(dataset_.end(), t, se_imu);
        }

        MRPT_LOG_INFO_STREAM("imu0: Loaded " << dat.rows() << " entries.");
    }

    // Debug: dump poses.
    for (unsigned int i = 0; i < cam_poses_.size(); i++)
    {
        mrpt::poses::CPose3D        p(cam_poses_[i]);
        mrpt::math::CMatrixDouble44 T;
        p.getHomogeneousMatrix(T);
        MRPT_LOG_DEBUG_STREAM(
            "cam" << i << " pose on vehicle: " << cam_poses_[i]
                  << "\nTransf. matrix:\n"
                  << T);
    }

    // Start at the dataset begin:
    dataset_next_ = dataset_.begin();

    MRPT_END
}  // end initialize()

void EurocDataset::spinOnce()
{
    MRPT_START

    ProfilerEntry tleg(profiler_, "spinOnce");

    // Starting time:
    if (!replay_started_)
    {
        replay_begin_time_ = mrpt::Clock::now();
        replay_started_    = true;
    }

    // get current replay time:
    const double t =
        mrpt::system::timeDifference(replay_begin_time_, mrpt::Clock::now()) *
        time_warp_scale_;

    // time in [ns]
    const euroc_timestamp_t tim =
        static_cast<euroc_timestamp_t>(t * 1e9) + dataset_.begin()->first;

    if (dataset_next_ == dataset_.end())
    {
        MRPT_LOG_THROTTLE_INFO(
            10.0,
            "End of dataset reached! Nothing else to publish (CTRL+C to quit)");
        return;
    }

    // We have to publish all observations until "t":
    while (dataset_next_ != dataset_.end() && tim >= dataset_next_->first)
    {
        // Convert from dataset format:
        const auto obs_tim =
            mrpt::Clock::fromDouble(dataset_next_->first * 1e-9);

        std::visit(
            overloaded{[&](std::monostate&) {
                           THROW_EXCEPTION("Un-initialized entry!");
                       },
                       [&](SensorCamera& cam) {
                           build_dataset_entry_obs(cam);
                           cam.obs->timestamp = obs_tim;
                           this->sendObservationsToFrontEnds(cam.obs);
                           cam.obs.reset();  // free mem
                       },
                       [&](SensorIMU& imu) {
                           build_dataset_entry_obs(imu);
                           imu.obs->timestamp = obs_tim;
                           this->sendObservationsToFrontEnds(imu.obs);
                           imu.obs.reset();  // free mem
                       }},
            dataset_next_->second);

        // Advance:
        ++dataset_next_;
    }

    // Read ahead to save delays in the next iteration:
    {
        ProfilerEntry tle(profiler_, "spinOnce.read_ahead");

        const unsigned int READ_AHEAD_COUNT = 15;
        auto               peeker           = dataset_next_;
        ++peeker;
        for (unsigned int i = 0;
             i < READ_AHEAD_COUNT && peeker != dataset_.end(); ++i, ++peeker)
        {
            //
            std::visit(
                overloaded{
                    [&](std::monostate&) {
                        THROW_EXCEPTION("Un-initialized entry!");
                    },
                    [&](SensorCamera& cam) { build_dataset_entry_obs(cam); },
                    [&](SensorIMU& imu) { build_dataset_entry_obs(imu); }},
                peeker->second);
        }
    }

    MRPT_END
}

void EurocDataset::build_dataset_entry_obs(SensorCamera& s)
{
    if (s.obs) return;  // already done

    ProfilerEntry tleg(profiler_, "build_obs_img");

    auto obs         = mrpt::obs::CObservationImage::Create();
    obs->sensorLabel = s.sensor_name;

    const auto f = seq_dir_ + s.img_file_name;
    obs->image.setExternalStorage(f);

    // Use this thread time to load images from disk, instead of
    // delegating it to the first use of the image in the consumer:
    obs->image.forceLoad();

    obs->cameraParams = cam_intrinsics_[s.cam_idx];
    obs->setSensorPose(mrpt::poses::CPose3D(cam_poses_[s.cam_idx]));

    s.obs = mrpt::ptr_cast<mrpt::obs::CObservation>::from(obs);
}

void EurocDataset::build_dataset_entry_obs(SensorIMU& s)
{
    using namespace mrpt::obs;

    if (s.obs) return;  // already done

    ProfilerEntry tleg(profiler_, "build_obs_imu");

    MRPT_TODO("Port to CObservationIMU::CreateAlloc() with mem pool");

    auto obs         = CObservationIMU::Create();
    obs->sensorLabel = s.sensor_name;

    obs->dataIsPresent[IMU_WX]    = true;
    obs->dataIsPresent[IMU_WY]    = true;
    obs->dataIsPresent[IMU_WZ]    = true;
    obs->dataIsPresent[IMU_X_ACC] = true;
    obs->dataIsPresent[IMU_Y_ACC] = true;
    obs->dataIsPresent[IMU_Z_ACC] = true;

    obs->rawMeasurements[IMU_WX]    = s.wx;
    obs->rawMeasurements[IMU_WY]    = s.wy;
    obs->rawMeasurements[IMU_X_ACC] = s.accx;
    obs->rawMeasurements[IMU_Y_ACC] = s.accy;
    obs->rawMeasurements[IMU_Z_ACC] = s.accz;

    s.obs = mrpt::ptr_cast<mrpt::obs::CObservation>::from(obs);
}
