# fake-qcrilmsgtunnel

This service connects to vendor.qti.hardware.radio.qcrilhook@1.0::IQtiOemHook
and partially reproduces the Android QtiTelephony interaction with qcrilNrd
during system initialization.

The service establishes a connection via HIDL and monitors Sailfish oFono for
SIM unlock events. Once the SIM is unlocked, an "ATEL ready" message is sent to
qcrilNrd. This process is required on Sony Nagara devices to enable SMS
reception.