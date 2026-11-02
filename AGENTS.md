
## Local source ownership

Each Boot/App/Transfer owns all sources, headers and build tools needed to compile.
No sibling checkout, absolute source path or parent-directory include is permitted.
Use hal/can, hal/device, hal/image, hal/flash and hal/isp by function; do not add
common or version-number directories. Root headers remain local editable contracts.
Project orchestration belongs in custom_func.c hardware_init()/loop(), not a
separate runtime entry source. Preserve role-specific ordering and SC bindings.
Keep local Boot/App contracts compatible; App-only builds cannot modify Boot.

Standalone CAN Transfer owns one exact standard-data RX FIFO0 rule from
can_transfer_config.h. Never copy Boot control/block rules into RSCANlite.
CS+ pre/post hooks verify local configuration and the actual linked map/MOT;
keep these checks aligned with any intentional filter changes.
