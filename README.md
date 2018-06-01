# An ROS2 RMW implementation on top of Zhe

This is a proof-of-concept implementation of the core functionality of the ROS2 RMW interface layer on top of [*Zhe*](https://github.com/atolab/zhe). It currently hardcodes the use of the example UDP platform code and the example p2p configuration of *Zhe*.

## Building

Clone the repository inside the ROS2 sources (so it sits alongside the various RMW layers that are part of the standard ROS2 download, such as rmw\_opensplice and rmw\_fastrtps). That should suffice to have it included in the standard ROS2 build process, provided package configuration succeeds.

My cmake skills being what they are, getting the configuration to work will require setting an environment variable ```ZHE_HOME``` to point to the location where an installed Zhe static library & its headers is located. That is, it expects to find among other files:

````
${ZHE_HOME}/lib/libzhe.a
${ZHE_HOME}/include/zhe/zhe.h
${ZHE_HOME}/include/zhe/platform-udp.h
````

Cmake seems to furthermore require setting ```Zhe_DIR=${ZHE_HOME}``` to locate the package configuration file, as well as (possibly) setting ```fastcdr_DIR``` to find that package. For the [Fast-Buffers](https://github.com/eProsima/Fast-Buffers), I have had success in using the one included in the standard ROS2 build (present in because of the default RMW implememtation, rmw\_fastrtps), by setting it to ```$PWD/install/lib/fastcdr/cmake``` in the ros2\_ws directory where one invokes ament to build everything.

## Limitations

Obviously, all the limitations of *Zhe* itself apply.

The basic publish/subscribe and client/service mechanisms seem to work based on talker/lister and the add\_two\_ints tests after modification to avoid checking whether the service is available: introspection features are not implemented, as that requires *Zhe* to finally get support for historical data.

QoS on the reader respects the history depth setting, but ignores the writer side determines whether data is sent reliably in the current version of *Zhe*. QoS on the writer supports reliability, but not history because *Zhe* does not implement a writer history mechanism that allows dropping published values that have not yet been acknowledged. Transient local is not supported, again due to the lack of historical data support in *Zhe* at this point in time.

Resource IDs are computed by hashing the topic name module ZHE\_MAX\_RID, and so there is some risk of collisions, but that risk is small if there are not that far fewer topics than ZHE\_MAX\_RID. Good enough for a proof-of-concept.

Multiple ROS nodes in a single process should work, but this hasn't been tested.
