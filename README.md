# WLAN config

WLAN config values are stored in non volatile storage. NVS partition image is generated from nvs_data.csv and flashed automatically with the project.

File nvs_data.csv should contain at least the following namespace and entries:
```
key,type,encoding,value
wlan_config,namespace,,
ssid,data,string,<SSID>
psk,data,string,<WPA PSK>
```
