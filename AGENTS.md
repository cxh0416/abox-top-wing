# Repository scope

- Product identity is always `top_flying_wing`; do not derive it from a customer name.
- Keep public platform protocol separate from the vendor CAN implementation.
- Do not add meal-delivery locker, RS485, or legacy desktop-host business.
- Direction changes require explicit `stop` and confirmed `stopped` feedback.
- Never claim physical motion or OTA success from a build or MQTT acknowledgement alone.
- Release artifacts must preserve Boot `0x08000000`, App `0x08008000`, Boot State pages, and config page boundaries.
