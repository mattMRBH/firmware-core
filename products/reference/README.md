# Reference Product

This is a thin ESP-IDF product entrypoint used to validate the shared component
layout while the multi-product structure is still taking shape.

Current intent:

- provide a real ESP-IDF project root under `products/`
- keep `main/` intentionally small
- leave room for later product-specific BSP and composition code

Build example:

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/reference build
```
