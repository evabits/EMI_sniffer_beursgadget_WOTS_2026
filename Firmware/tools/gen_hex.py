# PlatformIO extra_script: after each build, emit an Intel HEX for SWD (J-Link/OpenOCD).
Import("env")  # type: ignore  # PlatformIO injects this

def generate_hex(source, target, env):
    objcopy = env.subst("$OBJCOPY")
    elf = env.subst("$BUILD_DIR/${PROGNAME}.elf")
    hexfile = env.subst("$BUILD_DIR/${PROGNAME}.hex")
    env.Execute(
        env.VerboseAction(
            f'"{objcopy}" -O ihex "{elf}" "{hexfile}"',
            f"Generating {hexfile}",
        )
    )

# Runs on every `pio run`, even if the ELF does not need to be relinked.
env.AddPostAction("buildprog", generate_hex)
