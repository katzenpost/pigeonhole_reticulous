# pigeonhole_reticulous

Apps for [Reticulous](https://github.com/reticulous) devices that use the Pigeonhole protocols: a
device is a client of a Pigeonhole replica on a Reticulum mesh, does the BACAP encryption itself,
and reads and writes boxes through the replica, so it shares groups with clients on the
Katzenpost mixnet through a bridge replica.

<p>
  <img src="esp-idf/test/lcd-sim/screens/04-chat.png" alt="Group Chat on a T-Deck screen: a group with kate, who uses katzenrns, and read receipts on our messages" width="320">
  <img src="esp-idf/test/lcd-sim/screens/07-join-voucher.png" alt="Group Chat while joining a group: our voucher code shown in large type" width="320">
</p>

The Group Chat app at the T-Deck's 320×240, over the namenlos bridge replica: a group with kate,
who uses katzenrns, and joining a group with a voucher code. The screens are drawn by
`esp-idf/test/lcd-sim`, which builds the app's screens on the desktop with the device's LVGL,
fonts and text entry, and runs the real chat client behind them.

Licensed under the GNU Affero General Public License, version 3 only (AGPL-3.0-only).
