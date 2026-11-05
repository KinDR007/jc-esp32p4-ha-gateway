"""PWM pins of JP1 (System › GPIO) as lights (the brightness is the duty) and WS2812 strips (colour, effects)."""

from __future__ import annotations

from typing import Any

from homeassistant.components.light import ATTR_BRIGHTNESS, ATTR_EFFECT, ATTR_RGB_COLOR, ColorMode, LightEntity, LightEntityFeature
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .coordinator import P4ConfigEntry
from .gpio import P4GpioEntity, add_gpio_entities


async def async_setup_entry(
    hass: HomeAssistant, entry: P4ConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    add_gpio_entities(entry.runtime_data, async_add_entities, "light", lambda c, g, mode, key: (P4GpioStrip if mode == "ws2812" else P4GpioPwm)(c, g, mode, key))


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


class P4GpioStrip(P4GpioEntity, LightEntity):
    """A WS2812 strip: one colour for every LED, brightness, the rainbow effect."""

    _attr_color_mode = ColorMode.RGB
    _attr_supported_color_modes = {ColorMode.RGB}
    _attr_supported_features = LightEntityFeature.EFFECT
    _attr_effect_list = ["none", "rainbow"]

    @property
    def _light(self) -> dict[str, Any]:
        pin = self.pin
        return (pin or {}).get("light") or {}

    @property
    def is_on(self) -> bool | None:
        return self._light.get("state") == "ON" if self._light else None

    @property
    def brightness(self) -> int | None:
        return self._light.get("brightness")

    @property
    def rgb_color(self) -> tuple[int, int, int] | None:
        c = self._light.get("color")
        return (c["r"], c["g"], c["b"]) if c else None

    @property
    def effect(self) -> str | None:
        return self._light.get("effect")

    async def async_turn_on(self, **kwargs: Any) -> None:
        body: dict[str, Any] = {"on": True}
        if ATTR_RGB_COLOR in kwargs:
            body["color"] = list(kwargs[ATTR_RGB_COLOR])
        if ATTR_BRIGHTNESS in kwargs:
            body["brightness"] = kwargs[ATTR_BRIGHTNESS]
        if ATTR_EFFECT in kwargs:
            body["effect"] = kwargs[ATTR_EFFECT]
        await self.set(body)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self.set({"on": False})
