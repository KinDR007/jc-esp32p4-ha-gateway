"""PWM pins of JP1 (System › GPIO) as lights: the brightness is the duty."""

from __future__ import annotations

from typing import Any

from homeassistant.components.light import ATTR_BRIGHTNESS, ColorMode, LightEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .coordinator import P4ConfigEntry
from .gpio import P4GpioEntity, add_gpio_entities


async def async_setup_entry(
    hass: HomeAssistant, entry: P4ConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    add_gpio_entities(entry.runtime_data, async_add_entities, "light", P4GpioPwm)


class P4GpioPwm(P4GpioEntity, LightEntity):
    """On while the duty is above 0; turning on without a brightness brings back the last duty."""

    _attr_color_mode = ColorMode.BRIGHTNESS
    _attr_supported_color_modes = {ColorMode.BRIGHTNESS}

    def __init__(self, coordinator, gpio: int, mode: str, key: str) -> None:
        super().__init__(coordinator, gpio, mode, key)
        self._last_duty = 100.0

    @property
    def _duty(self) -> float | None:
        pin = self.pin
        return pin.get("duty") if pin else None

    @property
    def is_on(self) -> bool | None:
        duty = self._duty
        return None if duty is None else duty > 0

    @property
    def brightness(self) -> int | None:
        duty = self._duty
        return None if duty is None else round(duty * 255 / 100)

    async def async_turn_on(self, **kwargs: Any) -> None:
        if ATTR_BRIGHTNESS in kwargs:
            duty = round(kwargs[ATTR_BRIGHTNESS] * 100 / 255, 1)
        else:
            duty = self._duty if self._duty else self._last_duty
        await self.set({"duty": duty})

    async def async_turn_off(self, **kwargs: Any) -> None:
        if self._duty:
            self._last_duty = self._duty
        await self.set({"duty": 0})
