# Products

This folder contains AirGradient product-specific ESP-IDF application roots.

Each product folder is a thin application layer that composes shared components
from `components/` into a buildable firmware project.

Typical contents of a product folder:

- `main/` - the ESP-IDF app entrypoint for that product
- `bsp/` - product-specific board support and hardware wiring
- `sdkconfig.defaults` - product-specific default configuration
- `CMakeLists.txt` - the ESP-IDF project root for that product

Current product folders:

- `reference/` - a thin reference product used to validate the project layout
  while the multi-product structure is still being defined

Guidelines:

- keep product folders thin
- put reusable code in `components/`
- keep product-specific wiring, BSP, and composition here
