# Configuring the Xpand Monitor

This document describes how to configure the Xpand monitor for use
with a Xpand cluster.

## Configuring the Monitor

Contrary to the other monitors of MaxScale, the Xpand monitor will
autonomously figure out the cluster configuration and for each Xpand
node create the corresponding MaxScale server object.

In order to do that, a _sufficient_ number of "bootstrap" server instances
must be specified in the MaxScale configuration file for the Xpand
monitor to start with. One server instance is in principle sufficient, but
if the corresponding node happens to be down when MaxScale starts, the
monitor will not be able to function.

```
[Bootstrap1]
type=server
address=10.2.224.101
port=3306
protocol=mariadbbackend

[Bootstrap2]
type=server
address=10.2.224.102
port=3306
protocol=mariadbbackend
```

The server configuration is identical with that of any other server, but since
these servers are _only_ used for bootstrapping the Xpand monitor it is
advisable to use names that clearly will identify them as such.

The actual Xpand monitor configuration looks as follows:
```
[Xpand]
type=monitor
module=xpandmon
servers=Bootstrap1, Bootstrap2
user=monitor_user
password=monitor_password
monitor_interval=2s
cluster_monitor_interval=60s
```

The mandatory parameters are the object type, the monitor module to use, the
list of servers to use for bootstrapping and the username and password to use
when connecting to the servers.

The `monitor_interval` parameter specifies how frequently the monitor should
ping the health check port of each server and the `cluster_monitor_interval`
specifies how frequently the monitor should do a complete cluster check, that
is, access the `system` tables of the Cluster for checking the Cluster
configuration. The default values are `2000` and `60000`, that is, 2 seconds
and 1 minute, respectively.

For each detected Xpand node a corresponding MaxScale server object will be
created, whose name is `@@<Monitor-Name>:node-<id>, where _Monitor-Name_
is the name of the monitor, in this example `Xpand` and _id_ is the node id
of the Xpand node. So, with a cluster of three nodes, the created servers
might be named like.

```
@@Xpand:node-2`
@@Xpand:node-3`
@@Xpand:node-7`
```
Note that as these are created at runtime and may disappear at any moment,
depending on changes happening in and made to the Xpand cluster, they
should never be referred to directly from service configurations. Instead,
services should refer to the monitor, as shown in the following:
```
[MyService]
type=service
router=readconnroute
user=service_user
password=service_password
cluster=Xpand
```
Instead of listing the servers of the service explicitly using the `servers`
parameter as usually is the case, the service refers to the Xpand monitor
using the `cluster` parameter. This will cause the service to use the Xpand
nodes that the Xpand monitor discovers at runtime.

For additional details, please consult the monitor
[documentation](../Monitors/Xpand-Monitor.md).