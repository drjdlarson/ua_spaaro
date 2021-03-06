/*
* Brian R Taylor
* brian.taylor@bolderflight.com
* 
* Copyright (c) 2021 Bolder Flight Systems Inc
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the “Software”), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#include "flight/telem.h"
#include "flight/global_defs.h"
#include "flight/hardware_defs.h"
#include "mavlink/mavlink.h"
#include "checksum/checksum.h"
#include "flight/msg.h"


namespace {
/* MavLink object */
bfs::MavLink<NUM_TELEM_PARAMS> telem_;
/* Temp storage for holding MissionItems as they are uploaded */
std::array<bfs::MissionItem, NUM_FLIGHT_PLAN_POINTS> temp_;
/* Stream periods, ms */
static constexpr int16_t RAW_SENS_STREAM_PERIOD_MS_ = 500;
static constexpr int16_t EXT_STATUS_STREAM_PERIOD_MS_ = 1000;
static constexpr int16_t RC_CHAN_STREAM_PERIOD_MS_ = 500;
static constexpr int16_t POS_STREAM_PERIOD_MS_ = 250;
static constexpr int16_t EXTRA1_STREAM_PERIOD_MS_ = 100;
static constexpr int16_t EXTRA2_STREAM_PERIOD_MS_ = 100;
/* Frame period, us */
static constexpr int16_t FRAME_PERIOD_US = FRAME_PERIOD_MS * 1000;
/* Parameter */
int32_t param_idx_;
static constexpr uint8_t PARAM_STORE_HEADER[] = {'B', 'F', 'S'};
static constexpr std::size_t PARAM_STORE_SIZE = sizeof(PARAM_STORE_HEADER) +
                                                NUM_TELEM_PARAMS *
                                                sizeof(float) +
                                                sizeof(uint16_t);
uint8_t param_buf[PARAM_STORE_SIZE];
bfs::Fletcher16 param_checksum;
uint16_t chk_computed, chk_read;
uint8_t chk_buf[2];
/* Effector */
std::array<int16_t, 16> effector_;
int NUM_SBUS = std::min(static_cast<std::size_t>(NUM_SBUS_CH),
                        effector_.size() - NUM_PWM_PINS);
}  // namespace

void TelemInit(const AircraftConfig &cfg, TelemData * const ptr) {
  if (!ptr) {return;}
  /* Config */
  telem_.hardware_serial(cfg.telem.bus);
  telem_.gnss_serial(cfg.sensor.gnss.bus);
  telem_.aircraft_type(cfg.telem.aircraft_type);
  telem_.mission(ptr->flight_plan.data(), ptr->flight_plan.size(),
                 temp_.data());
  telem_.fence(ptr->fence.data(), ptr->fence.size());
  telem_.rally(ptr->rally.data(), ptr->rally.size());
  /* Load the telemetry parameters from EEPROM */
  for (std::size_t i = 0; i < PARAM_STORE_SIZE; i++) {
    param_buf[i] = EEPROM.read(i);
  }
  /* Check whether the parameter store has been initialized */
  /* If it hasn't... */
  if ((param_buf[0] != PARAM_STORE_HEADER[0]) ||
      (param_buf[1] != PARAM_STORE_HEADER[1]) ||
      (param_buf[2] != PARAM_STORE_HEADER[2])) {
    MsgInfo("Parameter storage not initialized, initializing...");
    /* Set the header */
    param_buf[0] = PARAM_STORE_HEADER[0];
    param_buf[1] = PARAM_STORE_HEADER[1];
    param_buf[2] = PARAM_STORE_HEADER[2];
    /* Zero out the parameters */
    for (std::size_t i = 0; i < NUM_TELEM_PARAMS * sizeof(float); i++) {
      param_buf[i + 3] = 0;
    }
    /* Compute the checksum */
    chk_computed = param_checksum.Compute(param_buf,
                                          sizeof(PARAM_STORE_HEADER) +
                                          NUM_TELEM_PARAMS * sizeof(float));
    chk_buf[0] = static_cast<uint8_t>(chk_computed >> 8);
    chk_buf[1] = static_cast<uint8_t>(chk_computed);
    param_buf[PARAM_STORE_SIZE - 2] = chk_buf[0];
    param_buf[PARAM_STORE_SIZE - 1] = chk_buf[1];
    /* Write to EEPROM */
    for (std::size_t i = 0; i < PARAM_STORE_SIZE; i++) {
      EEPROM.write(i, param_buf[i]);
    }
    MsgInfo("done.\n");
  /* If it has been initialized */
  } else {
    /* Check the checksum */
    chk_computed = param_checksum.Compute(param_buf,
                                          sizeof(PARAM_STORE_HEADER) +
                                          NUM_TELEM_PARAMS * sizeof(float));
    chk_buf[0] = param_buf[PARAM_STORE_SIZE - 2];
    chk_buf[1] = param_buf[PARAM_STORE_SIZE - 1];
    chk_read = static_cast<uint16_t>(chk_buf[0]) << 8 |
               static_cast<uint16_t>(chk_buf[1]);
    if (chk_computed != chk_read) {
      /* Parameter store corrupted, reset and warn */
      MsgWarning("Parameter storage corrupted, resetting...");
      /* Set the header */
      param_buf[0] = PARAM_STORE_HEADER[0];
      param_buf[1] = PARAM_STORE_HEADER[1];
      param_buf[2] = PARAM_STORE_HEADER[2];
      /* Zero out the parameters */
      for (std::size_t i = 0; i < NUM_TELEM_PARAMS * sizeof(float); i++) {
        param_buf[i + 3] = 0;
      }
      /* Compute the checksum */
      chk_computed = param_checksum.Compute(param_buf,
                                            sizeof(PARAM_STORE_HEADER) +
                                            NUM_TELEM_PARAMS * sizeof(float));
      chk_buf[0] = static_cast<uint8_t>(chk_computed >> 8);
      chk_buf[1] = static_cast<uint8_t>(chk_computed);
      param_buf[PARAM_STORE_SIZE - 2] = chk_buf[0];
      param_buf[PARAM_STORE_SIZE - 1] = chk_buf[1];
      /* Write to EEPROM */
      for (std::size_t i = 0; i < PARAM_STORE_SIZE; i++) {
        EEPROM.write(i, param_buf[i]);
      }
      MsgInfo("done.\n");
    } else {
      /* Copy parameter data to global defs */
      memcpy(ptr->param.data(), param_buf + sizeof(PARAM_STORE_HEADER),
             NUM_TELEM_PARAMS * sizeof(float));
      /* Update the parameter values in MAV Link */
      telem_.params(ptr->param);
    }
  }
  /* Begin communication */
  telem_.Begin(cfg.telem.baud);
  /* Data stream rates */
  telem_.raw_sens_stream_period_ms(RAW_SENS_STREAM_PERIOD_MS_);
  telem_.ext_status_stream_period_ms(EXT_STATUS_STREAM_PERIOD_MS_);
  telem_.rc_chan_stream_period_ms(RC_CHAN_STREAM_PERIOD_MS_);
  telem_.pos_stream_period_ms(POS_STREAM_PERIOD_MS_);
  telem_.extra1_stream_period_ms(EXTRA1_STREAM_PERIOD_MS_);
  telem_.extra2_stream_period_ms(EXTRA2_STREAM_PERIOD_MS_);
}
void TelemUpdate(const AircraftData &data, TelemData * const ptr) {
  if (!ptr) {return;}
  /* System data */
  telem_.sys_time_us(data.sys.sys_time_us);
  telem_.cpu_load(data.sys.frame_time_us, FRAME_PERIOD_US);
  telem_.throttle_enabled(data.vms.motors_enabled);
  telem_.aircraft_mode(data.vms.mode);
  if (data.vms.motors_enabled) {
    telem_.aircraft_state(bfs::ACTIVE);
  } else {
    telem_.aircraft_state(bfs::STANDBY);
  }
  /* Installed sensors */
  telem_.accel_installed(true);
  telem_.gyro_installed(true);
  telem_.mag_installed(true);
  telem_.static_pres_installed(true);
  telem_.diff_pres_installed(data.sensor.pitot_static_installed);
  telem_.gnss_installed(true);
  telem_.inceptor_installed(true);
  /* Battery data */
  #if defined(__FMU_R_V2__)
  telem_.battery_volt(data.vms.battery.voltage_v);
  telem_.battery_current_ma(data.vms.battery.current_ma);
  telem_.battery_consumed_mah(data.vms.battery.consumed_mah);
  telem_.battery_remaining_prcnt(data.vms.battery.remaining_prcnt);
  telem_.battery_remaining_time_s(data.vms.battery.remaining_time_s);
  #endif
  #if defined(__FMU_R_V1__)
  telem_.battery_volt(data.sys.input_volt);
  #endif
  /* IMU data */
  telem_.accel_healthy(data.sensor.imu.imu_healthy);
  telem_.gyro_healthy(data.sensor.imu.imu_healthy);
  telem_.mag_healthy(data.sensor.imu.mag_healthy);
  telem_.imu_accel_x_mps2(data.sensor.imu.accel_mps2[0]);
  telem_.imu_accel_y_mps2(data.sensor.imu.accel_mps2[1]);
  telem_.imu_accel_z_mps2(data.sensor.imu.accel_mps2[2]);
  telem_.imu_gyro_x_radps(data.sensor.imu.gyro_radps[0]);
  telem_.imu_gyro_y_radps(data.sensor.imu.gyro_radps[1]);
  telem_.imu_gyro_z_radps(data.sensor.imu.gyro_radps[2]);
  telem_.imu_mag_x_ut(data.sensor.imu.mag_ut[0]);
  telem_.imu_mag_y_ut(data.sensor.imu.mag_ut[1]);
  telem_.imu_mag_z_ut(data.sensor.imu.mag_ut[2]);
  telem_.imu_die_temp_c(data.sensor.imu.die_temp_c);
  /* GNSS data */
  telem_.gnss_healthy(data.sensor.gnss.healthy);
  telem_.gnss_fix(data.sensor.gnss.fix);
  telem_.gnss_num_sats(data.sensor.gnss.num_sats);
  telem_.gnss_lat_rad(data.sensor.gnss.lat_rad);
  telem_.gnss_lon_rad(data.sensor.gnss.lon_rad);
  telem_.gnss_alt_msl_m(data.sensor.gnss.alt_msl_m);
  telem_.gnss_alt_wgs84_m(data.sensor.gnss.alt_wgs84_m);
  telem_.gnss_hdop(data.sensor.gnss.hdop);
  telem_.gnss_vdop(data.sensor.gnss.vdop);
  telem_.gnss_track_rad(data.sensor.gnss.track_rad);
  telem_.gnss_spd_mps(data.sensor.gnss.spd_mps);
  telem_.gnss_horz_acc_m(data.sensor.gnss.horz_acc_m);
  telem_.gnss_vert_acc_m(data.sensor.gnss.vert_acc_m);
  telem_.gnss_vel_acc_mps(data.sensor.gnss.vel_acc_mps);
  telem_.gnss_track_acc_rad(data.sensor.gnss.track_acc_rad);
  /* Airdata */
  if (data.sensor.pitot_static_installed) {
    telem_.static_pres_healthy(data.sensor.static_pres.healthy);
    telem_.static_pres_pa(data.sensor.static_pres.pres_pa);
    telem_.static_pres_die_temp_c(data.sensor.static_pres.die_temp_c);
    telem_.diff_pres_healthy(data.sensor.diff_pres.healthy);
    telem_.diff_pres_pa(data.sensor.diff_pres.pres_pa);
    telem_.diff_pres_die_temp_c(data.sensor.diff_pres.die_temp_c);
  } else {
    telem_.static_pres_healthy(data.sensor.static_pres.healthy);
    telem_.static_pres_pa(data.sensor.static_pres.pres_pa);
    telem_.static_pres_die_temp_c(data.sensor.static_pres.die_temp_c);
  }
  /* Nav data */
  telem_.nav_lat_rad(data.nav.lat_rad);
  telem_.nav_lon_rad(data.nav.lon_rad);
  telem_.nav_alt_msl_m(data.nav.alt_msl_m);
  telem_.nav_alt_agl_m(data.nav.alt_rel_m);
  telem_.nav_north_pos_m(data.nav.ned_pos_m[0]);
  telem_.nav_east_pos_m(data.nav.ned_pos_m[1]);
  telem_.nav_down_pos_m(data.nav.ned_pos_m[2]);
  telem_.nav_north_vel_mps(data.nav.ned_vel_mps[0]);
  telem_.nav_east_vel_mps(data.nav.ned_vel_mps[1]);
  telem_.nav_down_vel_mps(data.nav.ned_vel_mps[2]);
  telem_.nav_gnd_spd_mps(data.nav.gnd_spd_mps);
  telem_.nav_ias_mps(data.nav.ias_mps);
  telem_.nav_pitch_rad(data.nav.pitch_rad);
  telem_.nav_roll_rad(data.nav.roll_rad);
  telem_.nav_hdg_rad(data.nav.heading_rad);
  telem_.nav_gyro_x_radps(data.nav.gyro_radps[0]);
  telem_.nav_gyro_y_radps(data.nav.gyro_radps[1]);
  telem_.nav_gyro_z_radps(data.nav.gyro_radps[2]);
  /* Effector */
  for (std::size_t i = 0; i < NUM_PWM_PINS; i++) {
    effector_[i] = data.vms.pwm.cnt[i];
  }
  for (std::size_t i = 0; i < NUM_SBUS; i++) {
    effector_[i + NUM_PWM_PINS] = data.vms.sbus.cnt[i];
  }
  telem_.effector(effector_);
  /* Inceptor */
  telem_.inceptor_healthy(!data.sensor.inceptor.failsafe);
  telem_.throttle_prcnt(data.vms.throttle_cmd_prcnt);
  telem_.inceptor(data.sensor.inceptor.ch);
  /* Mission */
  if (data.vms.waypoint_reached) {
    telem_.AdvanceMissionItem();
  }
  /* Update */
  telem_.Update();
  /* Params */
  param_idx_ = telem_.updated_param();
  if (param_idx_ >= 0) {
    /* Update the value in global defs */
    ptr->param[param_idx_] = telem_.param(param_idx_);
    /* Update the parameter buffer value */
    memcpy(param_buf + sizeof(PARAM_STORE_HEADER) + param_idx_ * sizeof(float),
           &ptr->param[param_idx_], sizeof(float));
    /* Compute a new checksum */
    chk_computed = param_checksum.Compute(param_buf,
                                          sizeof(PARAM_STORE_HEADER) +
                                          NUM_TELEM_PARAMS * sizeof(float));
    chk_buf[0] = static_cast<uint8_t>(chk_computed >> 8);
    chk_buf[1] = static_cast<uint8_t>(chk_computed);
    param_buf[PARAM_STORE_SIZE - 2] = chk_buf[0];
    param_buf[PARAM_STORE_SIZE - 1] = chk_buf[1];
    /* Write to EEPROM */
    for (std::size_t i = 0; i < sizeof(float); i++) {
      std::size_t addr = i + sizeof(PARAM_STORE_HEADER) +
                         param_idx_ * sizeof(float);
      EEPROM.write(addr, param_buf[addr]);
    }
    EEPROM.write(PARAM_STORE_SIZE - 2, param_buf[PARAM_STORE_SIZE - 2]);
    EEPROM.write(PARAM_STORE_SIZE - 1, param_buf[PARAM_STORE_SIZE - 1]);
  }
  /* Flight plan */
  ptr->waypoints_updated = telem_.mission_updated();
  ptr->current_waypoint = telem_.active_mission_item();
  ptr->num_waypoints = telem_.num_mission_items();
  /* Fence */
  ptr->fence_updated = telem_.fence_updated();
  ptr->num_fence_items = telem_.num_fence_items();
  /* Rally */
  ptr->rally_points_updated = telem_.rally_points_updated();
  ptr->num_rally_points = telem_.num_rally_points();
}
