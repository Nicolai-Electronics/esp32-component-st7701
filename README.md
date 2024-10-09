# ESP LCD ST7701

[![Component Registry](https://components.espressif.com/components/nicolaielectronics/esp_lcd_st7701/badge.svg)](https://components.espressif.com/components/nicolaielectronics/esp_lcd_st7701)

Implementation of the ST7701 LCD controller with esp_lcd component.

| LCD controller | Communication interface | Component name |                                   Link to datasheet                                   |
| :------------: | :---------------------: | :------------: | :-----------------------------------------------------------------------------------: |
|     ST7701     |        MIPI-DSI         | esp_lcd_st7701 | [PDF](https://orientdisplay.com/wp-content/uploads/2020/11/ST7701S.pdf)               |

**Note**: MIPI-DSI interface only supports ESP-IDF v5.3 and above versions.

For more information on LCD, please refer to the [LCD documentation](https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/lcd/index.html).

## Add to project

Packages from this repository are uploaded to [Espressif's component service](https://components.espressif.com/).
You can add them to your project via `idf.py add-dependancy`, e.g.

```
    idf.py add-dependency "nicolaielectronics/esp_lcd_st7701"
```

Alternatively, you can create `idf_component.yml`. More is in [Espressif's documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-component-manager.html).
