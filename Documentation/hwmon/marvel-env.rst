.. SPDX-License-Identifier: GPL-2.0

Kernel driver marvel-env
========================

Supported systems:

  * Marvel (EV7) AlphaServers: the GS1280, ES80 and ES47 families,
    booted from the SRM console

Author: Matt Turner <mattst88@gmail.com>

Description
-----------

The management firmware of these machines monitors temperatures, fans,
voltages and power supplies, and the SRM console describes each sensor in
its configuration tree (GCT). This driver asks the console to refresh the
readings and exports them.

The console has no sensor names, so the labels are built from the reporting
unit (such as ``CMM0`` or ``MBM/PBM``) and the sensor's position in that
unit. On systems with more than one drawer every label starts with the
cabinet and drawer numbers.

A refresh keeps the boot CPU in the console with interrupts disabled for up
to 300 ms per unit, so a unit is refreshed at most once every 10 seconds.

The firmware also reports every change of state as a system event machine
check. When one arrives, the driver refreshes all units and notifies
userspace through the alarm attributes.

Sysfs entries
-------------

The management firmware gives each temperature, fan and voltage sensor a
warning limit and a failure limit, and reports the sensor in the warning or
failure state when its reading reaches the limit. Neither state means that
the sensor is broken. Power supplies have no warning state and are reported
as voltage channels that only have a fault attribute.

=========================== =================================================
``temp[1-*]_input``         Temperature
``temp[1-*]_max``           Warning limit
``temp[1-*]_crit``          Failure limit
``temp[1-*]_max_alarm``     Temperature reached the warning limit
``temp[1-*]_crit_alarm``    Temperature reached the failure limit
``temp[1-*]_label``         Sensor label

``fan[1-*]_input``          Fan speed
``fan[1-*]_min``            Warning limit
``fan[1-*]_min_alarm``      Fan speed dropped to the warning limit
``fan[1-*]_fault``          Fan speed dropped to the failure limit, and the
                            firmware counts the fan as failed
``fan[1-*]_label``          Fan label

``in[0-*]_input``           Voltage
``in[0-*]_min_alarm``       Voltage is below the warning limit
``in[0-*]_max_alarm``       Voltage is above the warning limit
``in[0-*]_lcrit_alarm``     Voltage is below the failure limit
``in[0-*]_crit_alarm``      Voltage is above the failure limit
``in[0-*]_fault``           Power supply has failed
``in[0-*]_label``           Sensor or power supply label
=========================== =================================================

The firmware checks a voltage against limits on both sides of the nominal
voltage, but the console only describes the nominal voltage and one other
limit, so the voltage limits are not exported. Whether a voltage is too low
or too high comes from comparing it with the nominal voltage.

Sensors that the console reports as unknown or not present when the driver
loads are not exported. If a sensor becomes unknown later, reading its input,
alarm or fault attribute fails with ``-ENODATA``.
