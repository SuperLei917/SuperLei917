/****************************************************************************
 *
 *   Copyright (c) 2019-2021 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file AirspeedValidator.cpp
 * Estimates airspeed scale error (from indicated to calibrated airspeed), performes
 * checks on airspeed measurement input and reports airspeed valid or invalid.
 */

#include "AirspeedValidator.hpp"


void
AirspeedValidator::update_airspeed_validator(const airspeed_validator_update_data &input_data)
{
	// get indicated airspeed from input data (raw airspeed)
	_IAS = input_data.airspeed_indicated_raw;

	update_CAS_scale_estimated(input_data.lpos_valid, input_data.lpos_vx, input_data.lpos_vy, input_data.lpos_vz);
	update_CAS_scale_applied();
	update_CAS_TAS(input_data.air_pressure_pa, input_data.air_temperature_celsius);
	update_wind_estimator(input_data.timestamp, input_data.airspeed_true_raw, input_data.lpos_valid, input_data.lpos_vx,
			      input_data.lpos_vy,
			      input_data.lpos_vz, input_data.lpos_evh, input_data.lpos_evv, input_data.att_q);
	update_in_fixed_wing_flight(input_data.in_fixed_wing_flight);
	check_airspeed_innovation(input_data.timestamp, input_data.vel_test_ratio, input_data.mag_test_ratio);
	check_load_factor(input_data.accel_z);
	update_airspeed_valid_status(input_data.timestamp);
}

void
AirspeedValidator::reset_airspeed_to_invalid(const uint64_t timestamp)
{
	_airspeed_valid = false;
	_time_checks_failed = timestamp;
}

void
AirspeedValidator::update_wind_estimator(const uint64_t time_now_usec, float airspeed_true_raw, bool lpos_valid,
		float lpos_vx, float lpos_vy,
		float lpos_vz, float lpos_evh, float lpos_evv, const float att_q[4])
{
	_wind_estimator.update(time_now_usec);

	if (lpos_valid && _in_fixed_wing_flight) {

		Vector3f vI(lpos_vx, lpos_vy, lpos_vz);
		Quatf q(att_q);

		// airspeed fusion (with raw TAS)
		const Vector3f vel_var{Dcmf(q) *Vector3f{lpos_evh, lpos_evh, lpos_evv}};
		_wind_estimator.fuse_airspeed(time_now_usec, airspeed_true_raw, vI, Vector2f{vel_var(0), vel_var(1)});

		// sideslip fusion
		_wind_estimator.fuse_beta(time_now_usec, vI, q);
	}
}

// this function returns the current states of the wind estimator to be published in the airspeed module
airspeed_wind_s
AirspeedValidator::get_wind_estimator_states(uint64_t timestamp)
{
	airspeed_wind_s wind_est = {};

	wind_est.timestamp = timestamp;
	float wind[2];
	_wind_estimator.get_wind(wind);
	wind_est.windspeed_north = wind[0];
	wind_est.windspeed_east = wind[1];
	float wind_cov[2];
	_wind_estimator.get_wind_var(wind_cov);
	wind_est.variance_north = wind_cov[0];
	wind_est.variance_east = wind_cov[1];
	wind_est.tas_innov = _wind_estimator.get_tas_innov();
	wind_est.tas_innov_var = _wind_estimator.get_tas_innov_var();
	wind_est.beta_innov = _wind_estimator.get_beta_innov();
	wind_est.beta_innov_var = _wind_estimator.get_beta_innov_var();
	wind_est.tas_scale = _wind_estimator.get_tas_scale();
	wind_est.tas_scale_var = _wind_estimator.get_tas_scale_var();
	return wind_est;
}

void
AirspeedValidator::update_CAS_scale_estimated(bool lpos_valid, float vx, float vy, float vz)
{
	if (!_in_fixed_wing_flight) {
		return;
	}

	// reset every 100s as we assume that all the samples for current check are in similar wind conditions
	if (hrt_elapsed_time(&_begin_current_scale_check) > 100_s) {
		reset_CAS_scale_check();
	}


	const float course_over_ground_rad = matrix::wrap_2pi(atan2f(vy, vx));
	const int index = int(SCALE_CHECK_SAMPLES * course_over_ground_rad / (2.f * M_PI_F));

	_scale_check_groundspeed[index] = sqrt(vx * vx + vy * vy + vz * vz);
	_scale_check_TAS[index] = _TAS;

	float ground_speed_sum = 0.f;
	float TAS_sum = 0.f;

	for (int i = 0; i < SCALE_CHECK_SAMPLES; i++) {
		if (_scale_check_TAS[i] < FLT_EPSILON) {
			_scale_check_TAS[i] = NAN; //needed to find uninitialized array
			_scale_check_groundspeed[i] = NAN;
		}

		if (_scale_check_groundspeed[i] < FLT_EPSILON) {
			_scale_check_groundspeed[i] = NAN; //needed to find uninitialized array
		}

		ground_speed_sum += _scale_check_groundspeed[i];
		TAS_sum += _scale_check_TAS[i];
	}

	const bool full_array = PX4_ISFINITE(ground_speed_sum) && PX4_ISFINITE(TAS_sum);

	if (full_array) {
		const float average_ground_speed = ground_speed_sum / SCALE_CHECK_SAMPLES;
		const float average_TAS = TAS_sum / SCALE_CHECK_SAMPLES;
		const float average_TAS_with_scale = average_TAS * _wind_estimator.get_tas_scale();

		const float error_without_scale = abs(average_ground_speed - average_TAS);
		const float error_with_scale = abs(average_ground_speed - average_TAS_with_scale);

		// check passes if the average airspeed with the scale applied is closer to groundspeed than without
		if (error_with_scale < error_without_scale) {

			// constrain the scale update to max 0.01 at a time
			const float new_scale_constrained = math::constrain(_wind_estimator.get_tas_scale(), _CAS_scale_estimated - 0.01f,
							    _CAS_scale_estimated + 0.01f);

			// this is an info only for the initial phase, should remove it once the scale estimator is validated
			PX4_INFO("_CAS_scale_estimated updated: %.2f --> %.2f", (double)_CAS_scale_estimated,
				 (double)new_scale_constrained);

			_CAS_scale_estimated = new_scale_constrained;
		}

		reset_CAS_scale_check();
	}
}

void
AirspeedValidator::reset_CAS_scale_check()
{

	// reset arrays to NAN
	for (int i = 0; i < SCALE_CHECK_SAMPLES; i++) {
		_scale_check_groundspeed[i] = NAN;
		_scale_check_TAS[i] = NAN;
	}

	_begin_current_scale_check = hrt_absolute_time();
}

void
AirspeedValidator::update_CAS_scale_applied()
{
	switch (_tas_scale_apply) {
	case 0:
	case 1:
		_CAS_scale_applied = _tas_scale_init;
		break;

	case 2:
		_CAS_scale_applied = _tas_scale_init;
		break;

	case 3:
		_CAS_scale_applied = _CAS_scale_estimated;
		break;
	}
}

void
AirspeedValidator::update_CAS_TAS(float air_pressure_pa, float air_temperature_celsius)
{
	_CAS = calc_CAS_from_IAS(_IAS, _CAS_scale_applied);
	_TAS = calc_TAS_from_CAS(_CAS, air_pressure_pa, air_temperature_celsius);
}

void
AirspeedValidator::check_airspeed_innovation(uint64_t time_now, float estimator_status_vel_test_ratio,
		float estimator_status_mag_test_ratio)
{
	// Check normalised innovation levels with requirement for continuous data and use of hysteresis
	// to prevent false triggering.

	if (_wind_estimator.get_wind_estimator_reset()) {
		_time_wind_estimator_initialized = time_now;
	}

	// reset states if we are not flying or wind estimator was just initialized/reset
	if (!_in_fixed_wing_flight || (time_now - _time_wind_estimator_initialized) < 10_s) {
		_innovations_check_failed = false;
		_time_last_tas_pass = time_now;

	} else {
		const float dt_s = math::constrain((time_now - _time_last_aspd_innov_check) / 1e6f, 0.01f, 0.2f); // limit to [100,5] Hz

		if ((estimator_status_vel_test_ratio < 1.f) && (estimator_status_mag_test_ratio < 1.f)) {
			// nav velocity data is likely good so airspeed innovations are able to be used
			// compute the ratio of innovation to gate size
			const float tas_test_ratio = _wind_estimator.get_tas_innov() * _wind_estimator.get_tas_innov()
						     / (fmaxf(_tas_gate, 1.0f) * fmaxf(_tas_gate, 1.f) * _wind_estimator.get_tas_innov_var());

			if (tas_test_ratio > _tas_innov_threshold) {
				_apsd_innov_integ_state += dt_s * (tas_test_ratio - _tas_innov_threshold); // integrate exceedance

			} else {
				// reset integrator used to trigger and record pass if integrator check is disabled
				_apsd_innov_integ_state = 0.f;

				if (_tas_innov_integ_threshold <= 0.f) {
					_time_last_tas_pass = time_now;
				}
			}

			if (_tas_innov_integ_threshold > 0.f && _apsd_innov_integ_state < _tas_innov_integ_threshold) {
				_time_last_tas_pass = time_now;
			}
		}

		_innovations_check_failed = (time_now - _time_last_tas_pass) > TAS_INNOV_FAIL_DELAY;
	}

	_time_last_aspd_innov_check = time_now;
}


void
AirspeedValidator::check_load_factor(float accel_z)
{
	// Check if the airpeed reading is lower than physically possible given the load factor

	if (_in_fixed_wing_flight) {

		float max_lift_ratio = fmaxf(_CAS, 0.7f) / fmaxf(_airspeed_stall, 1.0f);
		max_lift_ratio *= max_lift_ratio;
		_load_factor_ratio = 0.95f * _load_factor_ratio + 0.05f * (fabsf(accel_z) / 9.81f) / max_lift_ratio;
		_load_factor_ratio = math::constrain(_load_factor_ratio, 0.25f, 2.0f);
		_load_factor_check_failed = (_load_factor_ratio > 1.1f);

	} else {
		_load_factor_ratio = 0.5f; // reset if not in fixed-wing flight (and not in takeoff condition)
	}
}


void
AirspeedValidator::update_airspeed_valid_status(const uint64_t timestamp)
{
	if (_innovations_check_failed || _load_factor_check_failed) {
		// either innovation or load factor check failed, so record timestamp
		_time_checks_failed = timestamp;

	} else if (!_innovations_check_failed && !_load_factor_check_failed) {
		// both innovation or load factor checks must pass to declare airspeed good
		_time_checks_passed = timestamp;
	}

	if (_airspeed_valid) {
		// A simultaneous load factor and innovaton check fail makes it more likely that a large
		// airspeed measurement fault has developed, so a fault should be declared immediately
		const bool both_checks_failed = (_innovations_check_failed && _load_factor_check_failed);

		// Because the innovation and load factor checks are subject to short duration false positives
		// a timeout period is applied.
		const bool single_check_fail_timeout = (timestamp - _time_checks_passed) > _checks_fail_delay * 1_s;

		if (both_checks_failed || single_check_fail_timeout) {

			_airspeed_valid = false;
		}

	} else if (_checks_clear_delay > 0.f && (timestamp - _time_checks_failed) > _checks_clear_delay * 1_s) {
		// disable the re-enabling if the clear delay is negative
		_airspeed_valid = true;
	}
}
