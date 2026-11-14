# Reusing ESPHome recipes in Matter Builder

Put the pin assignments, signal polarity, timeouts and local rules in the
recipe. Add one `# Setup:` line per manual step. The catalog and setup wizard
show those lines in order. When the user changes a pin, the connection uses the
new value. Update the setup text as well if sharing the edited recipe.

| Example | Reusable behavior |
| --- | --- |
| [LD2410 occupancy](../examples/mmwave-presence.yaml) | Use a digital output for presence. Other sensors with a compatible active-high output can use the same connection. The radar's settings remain on the radar. |
| [Humidity fan](../examples/humidity-fan.yaml) | Use separate start and stop thresholds to avoid rapid relay switching. The same rules work for other temperature or humidity triggers. |
| [Single-zone watering](../examples/single-zone-irrigation.yaml) | Give the relay a firmware timeout so manual and scheduled starts both stop automatically. Verify the device clock before enabling the schedule. |
| [Garage opener](../examples/garage-door-pulse.yaml) | Use a timed relay for an impulse input and a separate contact for feedback. A similar pair works with a doorbell or other equipment with an impulse input. |
| [Yamaha receiver](../examples/yamaha-receiver-ir.yaml) | Record a key with an IR receiver and copy the received NEC code into an IR send connection. Several commands can share the same transmitter. |
| [SCD41](../examples/scd41-air-quality.yaml) | Reuse the I²C device setup and Sensirion word CRC from the SHT4x. Keep the sensor-specific start, delay, read and retry sequence in its driver. The driver also supplies temperature and humidity to local rules. |
| [Optical or S0 pulse meter](../examples/energy-pulse-meter.yaml) | The GPIO pulse path handles either edge and ignores edges less than 10 ms apart. Pulses per kWh sets the conversion. The first rate needs two pulses. The cumulative total is checkpointed about once per minute while changing, so a sudden outage can lose recent pulses. |

The garage recipe exposes a switch and a contact sensor. The LD2410 recipe
reports binary occupancy. The Yamaha recipe sends commands without appliance
state feedback. The SCD41 and pulse-meter recipes need firmware with their
drivers. The new recipes have been checked in code. Peripheral operation
and Matter controller interoperability still need hardware testing.
