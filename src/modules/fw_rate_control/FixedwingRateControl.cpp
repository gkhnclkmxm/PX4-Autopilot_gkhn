/****************************************************************************
 *
 *   Copyright (c) 2013-2019 PX4 Development Team. All rights reserved.
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

#include "FixedwingRateControl.hpp"

using namespace time_literals;
using namespace matrix;

using math::constrain;
using math::interpolate;
using math::radians;

FixedwingRateControl::FixedwingRateControl(bool vtol) :
	ModuleParams(nullptr),
	ScheduledWorkItem(MODULE_NAME, px4::wq_configurations::nav_and_controllers),
	_actuator_controls_0_pub(vtol ? ORB_ID(actuator_controls_virtual_fw) : ORB_ID(actuator_controls_0)),
	_actuator_controls_status_pub(vtol ? ORB_ID(actuator_controls_status_1) : ORB_ID(actuator_controls_status_0)),
	_loop_perf(perf_alloc(PC_ELAPSED, MODULE_NAME": cycle"))
{
	/* fetch initial parameter values */
	parameters_update();

	_rate_ctrl_status_pub.advertise();
	_spoiler_setpoint_with_slewrate.setSlewRate(kSpoilerSlewRate);
	_flaps_setpoint_with_slewrate.setSlewRate(kFlapSlewRate);
}

FixedwingRateControl::~FixedwingRateControl()
{
	perf_free(_loop_perf);
}

bool
FixedwingRateControl::init()
{
	if (!_vehicle_angular_velocity_sub.registerCallback()) {
		PX4_ERR("callback registration failed");
		return false;
	}

	return true;
}

int
FixedwingRateControl::parameters_update()
{
	const Vector3f rate_p = Vector3f(_param_fw_rr_p.get(), _param_fw_pr_p.get(), _param_fw_yr_p.get());
	const Vector3f rate_i = Vector3f(_param_fw_rr_i.get(), _param_fw_pr_i.get(), _param_fw_yr_i.get());
	const Vector3f rate_d = Vector3f(_param_fw_rr_d.get(), _param_fw_pr_d.get(), _param_fw_yr_d.get());

	_rate_control.setGains(rate_p, rate_i, rate_d);

	_rate_control.setIntegratorLimit(
		Vector3f(_param_fw_rr_imax.get(), _param_fw_pr_imax.get(), _param_fw_yr_imax.get()));

	_rate_control.setFeedForwardGain(
		// set FF gains to 0 as we add the FF control outside of the rate controller
		Vector3f(0.f, 0.f, 0.f));


	return PX4_OK;
}

void
FixedwingRateControl::vehicle_manual_poll()
{
	if (_vcontrol_mode.flag_control_manual_enabled && !_vcontrol_mode.flag_control_climb_rate_enabled
	    && _in_fw_or_transition_wo_tailsitter_transition) {

		// Always copy the new manual setpoint, even if it wasn't updated, to fill the actuators with valid values
		if (_manual_control_setpoint_sub.copy(&_manual_control_setpoint)) {

			if (_vcontrol_mode.flag_control_rates_enabled &&
			    !_vcontrol_mode.flag_control_attitude_enabled) {

				// RATE mode we need to generate the rate setpoint from manual user inputs
				_rates_sp.timestamp = hrt_absolute_time();
				_rates_sp.roll = _manual_control_setpoint.roll * radians(_param_fw_acro_x_max.get());
				_rates_sp.pitch = -_manual_control_setpoint.pitch * radians(_param_fw_acro_y_max.get());
				_rates_sp.yaw = _manual_control_setpoint.yaw * radians(_param_fw_acro_z_max.get());
				_rates_sp.thrust_body[0] = (_manual_control_setpoint.throttle + 1.f) * .5f;

				_rate_sp_pub.publish(_rates_sp);

			} else {
				/* manual/direct control */
				_actuator_controls.control[actuator_controls_s::INDEX_ROLL] =
					_manual_control_setpoint.roll * _param_fw_man_r_sc.get() + _param_trim_roll.get();
				_actuator_controls.control[actuator_controls_s::INDEX_PITCH] =
					-_manual_control_setpoint.pitch * _param_fw_man_p_sc.get() + _param_trim_pitch.get();
				_actuator_controls.control[actuator_controls_s::INDEX_YAW] =
					_manual_control_setpoint.yaw * _param_fw_man_y_sc.get() + _param_trim_yaw.get();
				_actuator_controls.control[actuator_controls_s::INDEX_THROTTLE] = (_manual_control_setpoint.throttle + 1.f) * .5f;
			}
		}

	}
}

void
FixedwingRateControl::vehicle_land_detected_poll()
{
	if (_vehicle_land_detected_sub.updated()) {
		vehicle_land_detected_s vehicle_land_detected {};

		if (_vehicle_land_detected_sub.copy(&vehicle_land_detected)) {
			_landed = vehicle_land_detected.landed;
		}
	}
}

float FixedwingRateControl::get_airspeed_and_update_scaling()
{
	_airspeed_validated_sub.update();
	const bool airspeed_valid = PX4_ISFINITE(_airspeed_validated_sub.get().calibrated_airspeed_m_s)
				    && (hrt_elapsed_time(&_airspeed_validated_sub.get().timestamp) < 1_s);

	// if no airspeed measurement is available out best guess is to use the trim airspeed
	float airspeed = _param_fw_airspd_trim.get();

	if ((_param_fw_arsp_mode.get() == 0) && airspeed_valid) {
		/* prevent numerical drama by requiring 0.5 m/s minimal speed */
		airspeed = math::max(0.5f, _airspeed_validated_sub.get().calibrated_airspeed_m_s);

	} else {
		// VTOL: if we have no airspeed available and we are in hover mode then assume the lowest airspeed possible
		// this assumption is good as long as the vehicle is not hovering in a headwind which is much larger
		// than the stall airspeed
		if (_vehicle_status.is_vtol && _vehicle_status.vehicle_type == vehicle_status_s::VEHICLE_TYPE_ROTARY_WING
		    && !_vehicle_status.in_transition_mode) {
			airspeed = _param_fw_airspd_stall.get();
		}
	}

	/*
	 * For scaling our actuators using anything less than the stall
	 * speed doesn't make any sense - its the strongest reasonable deflection we
	 * want to do in flight and it's the baseline a human pilot would choose.
	 *
	 * Forcing the scaling to this value allows reasonable handheld tests.
	 */
	const float airspeed_constrained = constrain(constrain(airspeed, _param_fw_airspd_stall.get(),
					   _param_fw_airspd_max.get()), 0.1f, 1000.0f);

	_airspeed_scaling = (_param_fw_arsp_scale_en.get()) ? (_param_fw_airspd_trim.get() / airspeed_constrained) : 1.0f;

	return airspeed;
}

void FixedwingRateControl::Run()
{
	if (should_exit()) {
		_vehicle_angular_velocity_sub.unregisterCallback();
		exit_and_cleanup();
		return;
	}

	perf_begin(_loop_perf);

	// only run controller if angular velocity changed
	if (_vehicle_angular_velocity_sub.updated() || (hrt_elapsed_time(&_last_run) > 20_ms)) { //TODO rate!

		// only update parameters if they changed
		bool params_updated = _parameter_update_sub.updated();

		// check for parameter updates
		if (params_updated) {
			// clear update
			parameter_update_s pupdate;
			_parameter_update_sub.copy(&pupdate);

			// update parameters from storage
			updateParams();
			parameters_update();
		}

		float dt = 0.f;

		static constexpr float DT_MIN = 0.002f;
		static constexpr float DT_MAX = 0.04f;

		vehicle_angular_velocity_s vehicle_angular_velocity{};

		if (_vehicle_angular_velocity_sub.copy(&vehicle_angular_velocity)) {
			dt = math::constrain((vehicle_angular_velocity.timestamp_sample - _last_run) * 1e-6f, DT_MIN, DT_MAX);
			_last_run = vehicle_angular_velocity.timestamp_sample;
		}

		if (dt < DT_MIN || dt > DT_MAX) {
			const hrt_abstime time_now_us = hrt_absolute_time();
			dt = math::constrain((time_now_us - _last_run) * 1e-6f, DT_MIN, DT_MAX);
			_last_run = time_now_us;
		}

		vehicle_angular_velocity_s angular_velocity{};
		_vehicle_angular_velocity_sub.copy(&angular_velocity);
		float rollspeed = angular_velocity.xyz[0];
		float pitchspeed = angular_velocity.xyz[1];
		float yawspeed = angular_velocity.xyz[2];
		const Vector3f rates(rollspeed, pitchspeed, yawspeed);
		const Vector3f angular_accel{angular_velocity.xyz_derivative};

		if (_vehicle_status.is_vtol_tailsitter) {

			/* roll- and yawspeed have to be swaped */
			float helper = rollspeed;
			rollspeed = -yawspeed;
			yawspeed = helper;
		}


		// this is only to pass through flaps/spoiler setpoints, can be removed once flaps/spoilers
		// are handled outside of attitude/rate controller.
		// TODO remove it
		_att_sp_sub.update(&_att_sp);

		// vehicle status update must be before the vehicle_control_mode poll, otherwise rate sp are not published during whole transition
		_vehicle_status_sub.update(&_vehicle_status);
		const bool is_in_transition_except_tailsitter = _vehicle_status.in_transition_mode
				&& !_vehicle_status.is_vtol_tailsitter;
		const bool is_fixed_wing = _vehicle_status.vehicle_type == vehicle_status_s::VEHICLE_TYPE_FIXED_WING;
		_in_fw_or_transition_wo_tailsitter_transition =  is_fixed_wing || is_in_transition_except_tailsitter;

		_vehicle_control_mode_sub.update(&_vcontrol_mode);

		vehicle_land_detected_poll();

		vehicle_manual_poll();
		vehicle_land_detected_poll();

		/* if we are in rotary wing mode, do nothing */
		if (_vehicle_status.vehicle_type == vehicle_status_s::VEHICLE_TYPE_ROTARY_WING && !_vehicle_status.is_vtol) {
			_spoiler_setpoint_with_slewrate.setForcedValue(0.f);
			_flaps_setpoint_with_slewrate.setForcedValue(0.f);
			perf_end(_loop_perf);
			return;
		}

		controlFlaps(dt);
		controlSpoilers(dt);

		if (_vcontrol_mode.flag_control_rates_enabled) {

			const float airspeed = get_airspeed_and_update_scaling();

			/* reset integrals where needed */
			if (_rates_sp.reset_integral) {
				_rate_control.resetIntegral();
			}

			// Reset integrators if the aircraft is on ground or not in a state where the fw attitude controller is run
			if (_landed || !_in_fw_or_transition_wo_tailsitter_transition) {

				_rate_control.resetIntegral();
			}

			// update saturation status from control allocation feedback
			control_allocator_status_s control_allocator_status;

			if (_control_allocator_status_subs[_vehicle_status.is_vtol ? 1 : 0].update(&control_allocator_status)) {
				Vector<bool, 3> saturation_positive;
				Vector<bool, 3> saturation_negative;

				if (!control_allocator_status.torque_setpoint_achieved) {
					for (size_t i = 0; i < 3; i++) {
						if (control_allocator_status.unallocated_torque[i] > FLT_EPSILON) {
							saturation_positive(i) = true;

						} else if (control_allocator_status.unallocated_torque[i] < -FLT_EPSILON) {
							saturation_negative(i) = true;
						}
					}
				}

				// TODO: send the unallocated value directly for better anti-windup
				_rate_control.setSaturationStatus(saturation_positive, saturation_negative);
			}

			/* bi-linear interpolation over airspeed for actuator trim scheduling */
			float trim_roll = _param_trim_roll.get();
			float trim_pitch = _param_trim_pitch.get();
			float trim_yaw = _param_trim_yaw.get();

			if (airspeed < _param_fw_airspd_trim.get()) {
				trim_roll += interpolate(airspeed, _param_fw_airspd_min.get(), _param_fw_airspd_trim.get(),
							 _param_fw_dtrim_r_vmin.get(),
							 0.0f);
				trim_pitch += interpolate(airspeed, _param_fw_airspd_min.get(), _param_fw_airspd_trim.get(),
							  _param_fw_dtrim_p_vmin.get(),
							  0.0f);
				trim_yaw += interpolate(airspeed, _param_fw_airspd_min.get(), _param_fw_airspd_trim.get(),
							_param_fw_dtrim_y_vmin.get(),
							0.0f);

			} else {
				trim_roll += interpolate(airspeed, _param_fw_airspd_trim.get(), _param_fw_airspd_max.get(), 0.0f,
							 _param_fw_dtrim_r_vmax.get());
				trim_pitch += interpolate(airspeed, _param_fw_airspd_trim.get(), _param_fw_airspd_max.get(), 0.0f,
							  _param_fw_dtrim_p_vmax.get());
				trim_yaw += interpolate(airspeed, _param_fw_airspd_trim.get(), _param_fw_airspd_max.get(), 0.0f,
							_param_fw_dtrim_y_vmax.get());
			}

			/* add trim increment if flaps are deployed  */
			trim_roll += _flaps_setpoint_with_slewrate.getState() * _param_fw_dtrim_r_flps.get();
			trim_pitch += _flaps_setpoint_with_slewrate.getState() * _param_fw_dtrim_p_flps.get();

			// add trim increment from spoilers (only pitch)
			trim_pitch += _spoiler_setpoint_with_slewrate.getState() * _param_fw_dtrim_p_spoil.get();

			if (_vcontrol_mode.flag_control_rates_enabled) {
				_rates_sp_sub.update(&_rates_sp);

				const Vector3f body_rates_setpoint = Vector3f(_rates_sp.roll, _rates_sp.pitch, _rates_sp.yaw);

				/* Run attitude RATE controllers which need the desired attitudes from above, add trim */
				const Vector3f angular_acceleration_setpoint = _rate_control.update(rates, body_rates_setpoint, angular_accel, dt,
						_landed);

				float roll_feedforward = _param_fw_rr_ff.get() * _airspeed_scaling * body_rates_setpoint(0);
				float roll_u = angular_acceleration_setpoint(0) * _airspeed_scaling * _airspeed_scaling + roll_feedforward;
				_actuator_controls.control[actuator_controls_s::INDEX_ROLL] =
					(PX4_ISFINITE(roll_u)) ? math::constrain(roll_u + trim_roll, -1.f, 1.f) : trim_roll;

				float pitch_feedforward = _param_fw_pr_ff.get() * _airspeed_scaling * body_rates_setpoint(1);
				float pitch_u = angular_acceleration_setpoint(1) * _airspeed_scaling * _airspeed_scaling + pitch_feedforward;
				_actuator_controls.control[actuator_controls_s::INDEX_PITCH] =
					(PX4_ISFINITE(pitch_u)) ? math::constrain(pitch_u + trim_pitch, -1.f, 1.f) : trim_pitch;

				const float yaw_feedforward = _param_fw_yr_ff.get() * _airspeed_scaling * body_rates_setpoint(2);
				float yaw_u = angular_acceleration_setpoint(2) * _airspeed_scaling * _airspeed_scaling + yaw_feedforward;

				_actuator_controls.control[actuator_controls_s::INDEX_YAW] = (PX4_ISFINITE(yaw_u)) ? math::constrain(yaw_u + trim_yaw,
						-1.f, 1.f) : trim_yaw;

				if (!PX4_ISFINITE(roll_u) || !PX4_ISFINITE(pitch_u) || !PX4_ISFINITE(yaw_u)) {
					_rate_control.resetIntegral();
				}

				/* throttle passed through if it is finite */
				_actuator_controls.control[actuator_controls_s::INDEX_THROTTLE] =
					(PX4_ISFINITE(_rates_sp.thrust_body[0])) ? _rates_sp.thrust_body[0] : 0.0f;

				/* scale effort by battery status */
				if (_param_fw_bat_scale_en.get() &&
				    _actuator_controls.control[actuator_controls_s::INDEX_THROTTLE] > 0.1f) {

					if (_battery_status_sub.updated()) {
						battery_status_s battery_status{};

						if (_battery_status_sub.copy(&battery_status) && battery_status.connected && battery_status.scale > 0.f) {
							_battery_scale = battery_status.scale;
						}
					}

					_actuator_controls.control[actuator_controls_s::INDEX_THROTTLE] *= _battery_scale;
				}
			}

			// publish rate controller status
			rate_ctrl_status_s rate_ctrl_status{};
			_rate_control.getRateControlStatus(rate_ctrl_status);
			rate_ctrl_status.timestamp = hrt_absolute_time();

			_rate_ctrl_status_pub.publish(rate_ctrl_status);

		} else {
			// full manual
			_rate_control.resetIntegral();
		}

		// Add feed-forward from roll control output to yaw control output
		// This can be used to counteract the adverse yaw effect when rolling the plane
		_actuator_controls.control[actuator_controls_s::INDEX_YAW] += _param_fw_rll_to_yaw_ff.get()
				* constrain(_actuator_controls.control[actuator_controls_s::INDEX_ROLL], -1.0f, 1.0f);

		_actuator_controls.control[actuator_controls_s::INDEX_FLAPS] = _flaps_setpoint_with_slewrate.getState();
		_actuator_controls.control[actuator_controls_s::INDEX_SPOILERS] = _spoiler_setpoint_with_slewrate.getState();
		_actuator_controls.control[actuator_controls_s::INDEX_AIRBRAKES] = 0.f;
		// FIXME: this should use _vcontrol_mode.landing_gear_pos in the future
		_actuator_controls.control[actuator_controls_s::INDEX_LANDING_GEAR] = _manual_control_setpoint.aux3;

		/* lazily publish the setpoint only once available */
		_actuator_controls.timestamp = hrt_absolute_time();
		_actuator_controls.timestamp_sample = vehicle_angular_velocity.timestamp;

		/* Only publish if any of the proper modes are enabled */
		if (_vcontrol_mode.flag_control_rates_enabled ||
		    _vcontrol_mode.flag_control_attitude_enabled ||
		    _vcontrol_mode.flag_control_manual_enabled) {
			_actuator_controls_0_pub.publish(_actuator_controls);

			if (!_vehicle_status.is_vtol) {
				publishTorqueSetpoint(angular_velocity.timestamp_sample);
				publishThrustSetpoint(angular_velocity.timestamp_sample);
			}
		}

		updateActuatorControlsStatus(dt);
	}

	// backup schedule
	ScheduleDelayed(20_ms);

	perf_end(_loop_perf);
}

void FixedwingRateControl::publishTorqueSetpoint(const hrt_abstime &timestamp_sample)
{
	vehicle_torque_setpoint_s v_torque_sp = {};
	v_torque_sp.timestamp = hrt_absolute_time();
	v_torque_sp.timestamp_sample = timestamp_sample;
	v_torque_sp.xyz[0] = _actuator_controls.control[actuator_controls_s::INDEX_ROLL];
	v_torque_sp.xyz[1] = _actuator_controls.control[actuator_controls_s::INDEX_PITCH];
	v_torque_sp.xyz[2] = _actuator_controls.control[actuator_controls_s::INDEX_YAW];

	_vehicle_torque_setpoint_pub.publish(v_torque_sp);
}

void FixedwingRateControl::publishThrustSetpoint(const hrt_abstime &timestamp_sample)
{
	vehicle_thrust_setpoint_s v_thrust_sp = {};
	v_thrust_sp.timestamp = hrt_absolute_time();
	v_thrust_sp.timestamp_sample = timestamp_sample;
	v_thrust_sp.xyz[0] = _actuator_controls.control[actuator_controls_s::INDEX_THROTTLE];
	v_thrust_sp.xyz[1] = 0.0f;
	v_thrust_sp.xyz[2] = 0.0f;

	_vehicle_thrust_setpoint_pub.publish(v_thrust_sp);
}

void FixedwingRateControl::controlFlaps(const float dt)
{
	/* default flaps to center */
	float flap_control = 0.0f;

	/* map flaps by default to manual if valid */
	if (PX4_ISFINITE(_manual_control_setpoint.flaps) && _vcontrol_mode.flag_control_manual_enabled) {
		flap_control = _manual_control_setpoint.flaps;

	} else if (_vcontrol_mode.flag_control_auto_enabled) {

		switch (_att_sp.apply_flaps) {
		case vehicle_attitude_setpoint_s::FLAPS_OFF:
			flap_control = 0.0f; // no flaps
			break;

		case vehicle_attitude_setpoint_s::FLAPS_LAND:
			flap_control = _param_fw_flaps_lnd_scl.get();
			break;

		case vehicle_attitude_setpoint_s::FLAPS_TAKEOFF:
			flap_control = _param_fw_flaps_to_scl.get();
			break;
		}
	}

	// move the actual control value continuous with time, full flap travel in 1sec
	_flaps_setpoint_with_slewrate.update(math::constrain(flap_control, 0.f, 1.f), dt);
}

void FixedwingRateControl::controlSpoilers(const float dt)
{
	float spoiler_control = 0.f;

	if (_vcontrol_mode.flag_control_manual_enabled) {
		switch (_param_fw_spoilers_man.get()) {
		case 0:
			break;

		case 1:
			spoiler_control = PX4_ISFINITE(_manual_control_setpoint.flaps) ? _manual_control_setpoint.flaps : 0.f;
			break;

		case 2:
			spoiler_control = PX4_ISFINITE(_manual_control_setpoint.aux1) ? _manual_control_setpoint.aux1 : 0.f;
			break;
		}

	} else if (_vcontrol_mode.flag_control_auto_enabled) {
		switch (_att_sp.apply_spoilers) {
		case vehicle_attitude_setpoint_s::SPOILERS_OFF:
			spoiler_control = 0.f;
			break;

		case vehicle_attitude_setpoint_s::SPOILERS_LAND:
			spoiler_control = _param_fw_spoilers_lnd.get();
			break;

		case vehicle_attitude_setpoint_s::SPOILERS_DESCEND:
			spoiler_control = _param_fw_spoilers_desc.get();
			break;
		}
	}

	_spoiler_setpoint_with_slewrate.update(math::constrain(spoiler_control, 0.f, 1.f), dt);
}

void FixedwingRateControl::updateActuatorControlsStatus(float dt)
{
	for (int i = 0; i < 4; i++) {
		float control_signal;

		if (i <= actuator_controls_status_s::INDEX_YAW) {
			// We assume that the attitude is actuated by control surfaces
			// consuming power only when they move
			control_signal = _actuator_controls.control[i] - _control_prev[i];
			_control_prev[i] = _actuator_controls.control[i];

		} else {
			control_signal = _actuator_controls.control[i];
		}

		_control_energy[i] += control_signal * control_signal * dt;
	}

	_energy_integration_time += dt;

	if (_energy_integration_time > 500e-3f) {

		actuator_controls_status_s status;
		status.timestamp = _actuator_controls.timestamp;

		for (int i = 0; i < 4; i++) {
			status.control_power[i] = _control_energy[i] / _energy_integration_time;
			_control_energy[i] = 0.f;
		}

		_actuator_controls_status_pub.publish(status);
		_energy_integration_time = 0.f;
	}
}

int FixedwingRateControl::task_spawn(int argc, char *argv[])
{
	bool vtol = false;

	if (argc > 1) {
		if (strcmp(argv[1], "vtol") == 0) {
			vtol = true;
		}
	}

	FixedwingRateControl *instance = new FixedwingRateControl(vtol);

	if (instance) {
		_object.store(instance);
		_task_id = task_id_is_work_queue;

		if (instance->init()) {
			return PX4_OK;
		}

	} else {
		PX4_ERR("alloc failed");
	}

	delete instance;
	_object.store(nullptr);
	_task_id = -1;

	return PX4_ERROR;
}

int FixedwingRateControl::custom_command(int argc, char *argv[])
{
	return print_usage("unknown command");
}

int FixedwingRateControl::print_usage(const char *reason)
{
	if (reason) {
		PX4_WARN("%s\n", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
fw_rate_control is the fixed-wing rate controller.

)DESCR_STR");

	PRINT_MODULE_USAGE_NAME("fw_rate_control", "controller");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_ARG("vtol", "VTOL mode", true);
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();

	return 0;
}

extern "C" __EXPORT int fw_rate_control_main(int argc, char *argv[])
{
	return FixedwingRateControl::main(argc, argv);
}
