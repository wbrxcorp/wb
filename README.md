# wb
A frontend of [vm](https://github.com/shimarin/vm)

## systemd unit file

### for system

change ```br0``` to bridge interface name you want to use

/etc/systemd/system/vm@.service

```
[Unit]
Description=Virtual Machine for %i

[Service]
Type=notify
ExecStart=vm service --bridge=br0 --name=%i /var/vm/%i
KillMode=mixed
TimeoutStopSec=180

[Install]
WantedBy=multi-user.target
```

### for user

$HOME/.config/systemd/user/vm\@.service

```
[Unit]
Description=Virtual Machine for %i

[Service]
Type=notify
ExecStart=vm service --bridge=br0 --name=%i /var/vm/%i
KillMode=mixed
TimeoutStopSec=180

[Install]
WantedBy=multi-user.target
```
