# TOTP-only access to Pctl Manager

After initial enrollment, the current six-digit TOTP code is the credential for opening Pctl Manager. The app's parental-control management actions are available only after that launch gate succeeds. Rotating the TOTP secret also requires the current TOTP code; it no longer asks the app to read or re-enter the static console PIN for rotation.

## Important platform boundary

This does **not** replace Nintendo's system parental-control PIN globally. The TOTP check is implemented by this homebrew application. Horizon's own System Settings, the HOME-menu parental-controls screen, and Nintendo's native authentication applet continue to use Nintendo's registered PIN or their own supported authentication flow. A homebrew app cannot make those native screens accept a changing TOTP code just by changing its own code.

The first TOTP enrollment still requires the existing console PIN to verify that the person enrolling controls the current setup. If the TOTP secret or authenticator is lost, recovery still uses the documented local re-enrollment process and the console PIN.

The TOTP secret is stored on the SD card at `/switch/nx_pctl_totp.key`; this is an application access gate, not a security boundary against a person who can modify the SD card or run privileged homebrew.
