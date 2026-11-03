make -C $(pwd)/sysmodule/ -j11 DEVKITPRO=/opt/devkitpro/ ATMOSPHERE_ROOT="$(pwd)/Atmosphere" clean
git pull
make -C $(pwd)/sysmodule/ -j11 DEVKITPRO=/opt/devkitpro/ ATMOSPHERE_ROOT="$(pwd)/Atmosphere"
