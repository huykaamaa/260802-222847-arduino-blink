#include "globals.h"
#include "html.h"

// escape gia tri config truoc khi noi vao attribute value='...' - tranh mot dau nhay/ngoac
// operator go vao (vd trong mqtt_pass) lam vo cau truc form.
static String htmlEscape(const char *s) {
  String out;
  for (const char *p = s; *p; ++p) {
    switch (*p) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += *p;
    }
  }
  return out;
}

void handleRoot() {
  String html;
  // Trang nay dai ~11KB. Khong reserve() thi day la hon chuc lan realloc+memcpy tang dan moi
  // lan mo trang, moi lan bo lai mot lo block chet giua heap.
  html.reserve(12288);

  html += "<!DOCTYPE html><html><head>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>";
  html += "*{box-sizing:border-box;}";
  html += "body{margin:0;padding:15px;background:#f4f7fb;font-family:Arial,Helvetica,sans-serif;color:#233244;}";
  html += ".card{max-width:980px;margin:auto;background:#fff;padding:24px;border-radius:18px;box-shadow:0 6px 20px rgba(0,0,0,.12);}";
  html += "h2{text-align:center;margin:0 0 16px;color:#1565C0;}";
  html += "h3{margin:22px 0 12px;color:#1565C0;border-bottom:2px solid #dbeafe;padding-bottom:6px;}";
  html += "h3:first-of-type{margin-top:0;}";
  html += ".panel{background:#f8fbff;border:1px solid #dbeafe;border-radius:12px;padding:14px;margin-bottom:14px;}";
  html += ".sensor{border:1px solid #d8e3f0;border-radius:12px;padding:10px 12px;margin-bottom:10px;background:#fbfdff;}";
  html += ".row{display:flex;gap:12px;flex-wrap:wrap;margin-top:8px;align-items:center;}";
  html += ".row:first-child{margin-top:0;}";
  html += ".field{flex:1;min-width:220px;}";
  html += ".field label,.single label{display:block;font-weight:bold;margin-bottom:6px;color:#556270;font-size:14px;}";
  html += ".field input,.single input{width:100%;padding:10px;border:1px solid #bfc9d6;border-radius:8px;font-size:15px;background:#fff;}";
  // Phai co ca .single: dong tren cho .single input width:100% + padding:10px, neu khong
  // override o day thi 3 o tick (MQTT/OSC/DHCP) bi keo dan full chieu rong, day chu ra xa.
  html += ".field input[type=checkbox],.single input[type=checkbox]{width:auto;padding:0;margin-right:6px;transform:translateY(2px);}";
  html += ".single{margin:10px 0;}";
  html += "#d{background:#eef7ff;border-left:5px solid #2196F3;padding:12px;border-radius:10px;margin-bottom:18px;line-height:1.7;}";
  html += ".btn{width:100%;padding:14px;border:none;border-radius:10px;background:#2196F3;color:white;font-size:16px;font-weight:bold;cursor:pointer;margin-top:8px;}";
  html += ".btn:hover{background:#1976D2;}";
  html += ".btn-test{width:auto;background:#546E7A;padding:6px 16px;font-size:13px;margin-top:0;}";
  html += ".btn-test:hover{background:#37474F;}";
  html += ".note{font-size:13px;color:#64748b;margin-top:6px;}";
  html += "@media(max-width:600px){";
  html += ".card{padding:16px;}";
  html += ".field{min-width:100%;}";
  html += "h2{font-size:24px;}";
  html += "}";
  html += "</style>";

  html += "<script>";
  html += "function update(){fetch('/data').then(r=>r.text()).then(t=>document.getElementById('d').innerHTML=t);}";
  html += "setInterval(update,500);";
  html += "window.onload=update;";
  html += "</script>";
  html += "</head><body><div class='card'>";
  html += "<h2>GIÁ SÁCH</h2>";
  html += "<div id='d'>Loading...</div>";

  // Form rieng cho nut Test relay, dat ngoai #mainForm de khong long form trong form.
  html += "<form id='testRelayForm' action='/test_relay' method='POST'></form>";
  html += "<form id='mainForm' action='/save' method='POST'>";

  html += "<div class='panel'>";
  html += "<h3>Sensor / Relay</h3>";
  html += "<div class='note'>Relay 1 LUÔN ON (không phụ thuộc sách). 2 sensor độc lập (vị trí 1, 2): cả 2 có sách → relay 2 OFF. Lấy 1 cuốn (1 trong 2 sensor trống) → relay 2 ON. Lấy cả 2 cuốn → relay 2 tiếp tục ON.</div>";
  html += "<button class='btn btn-test' style='width:100%;margin-top:10px' type='submit' form='testRelayForm'>Test Relay (đảo cả 2 relay ~2 giây)</button>";
  html += "</div>";

  html += "<div class='panel'>";
  html += "<h3>Relay backup</h3>";
  html += "<div class='note'>Máy có sẵn 6 relay vật lý, chia thành 3 cặp. Cặp được tick sẽ nhận cùng tín hiệu relay 1/relay 2 tính từ trạng thái sách; cặp không tick luôn OFF. Tick nhiều cặp cùng lúc để chạy song song dự phòng; nếu 1 relay hỏng, bỏ tick cặp đó và tick cặp còn tốt, không cần nạp lại firmware.</div>";
  for (int p = 0; p < RELAY_PAIR_NUM; p++) {
    html += "<div class='single'><label><input type='checkbox' name='relay_pair" + String(p) + "' " + (relayPairEnable[p] ? "checked" : "") + "> Cặp " + String(p + 1) + " (relay " + String(2 * p + 1) + " = GPIO" + String(relayPins[2 * p]) + ", relay " + String(2 * p + 2) + " = GPIO" + String(relayPins[2 * p + 1]) + ")</label></div>";
  }
  html += "</div>";

  html += "<div class='panel'>";
  html += "<h3>MQTT</h3>";
  html += "<div class='single'><label><input type='checkbox' name='mqtt_enable' " + String(mqttEnabled ? "checked" : "") + "> Enable MQTT</label></div>";
  html += "<div class='single'><label>IP</label><input name='mqtt_ip' value='" + htmlEscape(mqttServer) + "'></div>";
  html += "<div class='single'><label>Port</label><input name='mqtt_port' value='" + String(mqttPort) + "'></div>";
  html += "<div class='single'><label>User</label><input name='mqtt_user' value='" + htmlEscape(mqttUser) + "'></div>";
  // KHONG do mqttPass ra HTML: trang "/" khong yeu cau dang nhap (de dashboard tu refresh
  // duoc), nen "value=" o day dong nghia voi ai xem duoc trang cung doc duoc mat khau broker
  // bang View Source. De trong = giu nguyen, giong o cach auth_pass ben duoi.
  html += "<div class='single'><label>Pass</label><input type='password' name='mqtt_pass' placeholder='(giữ nguyên nếu để trống)'></div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Topic - Trạng thái 1 (đủ sách)</label><input name='mqtt_topic_full' value='" + htmlEscape(mqttTopicFull) + "'></div>";
  html += "<div class='field'><label>Giá trị</label><input name='mqtt_val_full' value='" + htmlEscape(mqttValueFull) + "'></div>";
  html += "</div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Topic - Trạng thái 2 (lấy 1 cuốn)</label><input name='mqtt_topic_one' value='" + htmlEscape(mqttTopicOneTaken) + "'></div>";
  html += "<div class='field'><label>Giá trị</label><input name='mqtt_val_one' value='" + htmlEscape(mqttValueOneTaken) + "'></div>";
  html += "</div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Topic - Trạng thái 3 (lấy 2 cuốn)</label><input name='mqtt_topic_two' value='" + htmlEscape(mqttTopicTwoTaken) + "'></div>";
  html += "<div class='field'><label>Giá trị</label><input name='mqtt_val_two' value='" + htmlEscape(mqttValueTwoTaken) + "'></div>";
  html += "</div>";
  html += "<div class='note'>Mỗi trạng thái publish 1 lần (khi vừa chuyển vào) vào đúng topic riêng của nó, payload là giá trị tương ứng. Ô Pass để trống nghĩa là giữ nguyên mật khẩu đang dùng (mật khẩu không được hiển thị lại ở đây).</div>";
  html += "</div>";

  html += "<div class='panel'>";
  html += "<h3>OSC</h3>";
  html += "<div class='single'><label><input type='checkbox' name='osc_enable' " + String(oscEnabled ? "checked" : "") + "> Enable OSC</label></div>";
  html += "<div class='single'><label>IP</label><input name='osc_ip' value='" + htmlEscape(oscIp) + "'></div>";
  html += "<div class='single'><label>Port</label><input name='osc_port' value='" + String(oscPort) + "'></div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Địa chỉ - Trạng thái 1 (đủ sách)</label><input name='osc_address_full' value='" + htmlEscape(oscAddressFull) + "'></div>";
  html += "<div class='field'><label>Giá trị</label><input name='osc_value_full' value='" + String(oscValueFull) + "'></div>";
  html += "</div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Địa chỉ - Trạng thái 2 (lấy 1 cuốn)</label><input name='osc_address_one' value='" + htmlEscape(oscAddressOneTaken) + "'></div>";
  html += "<div class='field'><label>Giá trị</label><input name='osc_value_one' value='" + String(oscValueOneTaken) + "'></div>";
  html += "</div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Địa chỉ - Trạng thái 3 (lấy 2 cuốn)</label><input name='osc_address_two' value='" + htmlEscape(oscAddressTwoTaken) + "'></div>";
  html += "<div class='field'><label>Giá trị</label><input name='osc_value_two' value='" + String(oscValueTwoTaken) + "'></div>";
  html += "</div>";
  html += "<div class='note'>3 trạng thái là 3 message OSC độc lập, mỗi cái 1 địa chỉ + 1 giá trị riêng, bắn 1 lần khi vừa chuyển vào trạng thái đó.</div>";
  html += "</div>";

  html += "<div class='panel'>";
  html += "<h3>Debounce</h3>";
  html += "<div class='single'><label>Debounce (ms)</label><input name='debounce' value='" + String(debounceTime) + "'></div>";
  html += "<div class='note'>Dùng chung cho cả 2 sensor, mỗi sensor tự tính debounce riêng theo giá trị này.</div>";
  html += "</div>";

  html += "<div class='panel'>";
  html += "<h3>Heartbeat (gửi lại trạng thái định kỳ)</h3>";
  html += "<div class='single'><label>Chu kỳ (ms, 0 = tắt)</label><input name='heartbeat' value='" + String(heartbeatInterval) + "'></div>";
  html += "<div class='note'>MQTT (QoS0) và OSC (UDP) đều không đảm bảo gửi tới nơi - nếu đúng lúc đổi trạng thái mà mạng chập chờn, bên nhận có thể bị lệch cho tới lần đổi kế tiếp. Heartbeat gửi lại trạng thái sách hiện tại theo chu kỳ này để tự đồng bộ lại.</div>";
  html += "</div>";

  html += "<div class='panel'>";
  html += "<h3>Mạng (ETH)</h3>";
  html += "<div class='single'><label><input type='checkbox' name='eth_static_first' " + String(ethUseStaticFirst ? "checked" : "") + "> Ưu tiên IP tĩnh (bỏ qua DHCP)</label></div>";
  html += "<div class='single'><label>IP</label><input name='eth_ip' value='" + htmlEscape(ethStaticIp) + "'></div>";
  html += "<div class='single'><label>Gateway</label><input name='eth_gw' value='" + htmlEscape(ethStaticGateway) + "'></div>";
  html += "<div class='single'><label>Netmask</label><input name='eth_mask' value='" + htmlEscape(ethStaticNetmask) + "'></div>";
  html += "<div class='note'>Bỏ tick: thiết bị thử DHCP trước (tối đa 10s lúc boot), chỉ dùng IP tĩnh khi DHCP thất bại. Tick: dùng IP tĩnh ngay từ đầu, bỏ qua DHCP hoàn toàn (boot nhanh hơn); nếu IP tĩnh nhập sai thì tự động lùi về DHCP. Đổi giá trị ở đây cần reboot board mới áp dụng.</div>";
  html += "</div>";

  html += "<div class='panel'>";
  html += "<h3>Admin Auth</h3>";
  html += "<div class='single'><label>Username</label><input name='auth_user' placeholder='(giữ nguyên nếu để trống)'></div>";
  html += "<div class='single'><label>Password</label><input type='password' name='auth_pass' placeholder='(giữ nguyên nếu để trống)'></div>";
  html += "<div class='note'>Bắt buộc (HTTP Basic Auth) để bấm Save Settings hoặc dùng các nút Test. Nên đổi khỏi mặc định admin/admin càng sớm càng tốt.</div>";
  html += "</div>";

  html += "<input class='btn' type='submit' value='SAVE SETTINGS'>";
  html += "</form>";

  html += "<div class='panel'>";
  html += "<h3>Test Settings</h3>";
  html += "<form action='/test_mqtt' method='POST' style='margin-bottom:10px;'><input class='btn' type='submit' value='Test MQTT (trạng thái 1→2→3)'></form>";
  html += "<form action='/test_osc' method='POST'><input class='btn' type='submit' value='Test OSC (trạng thái 1→2→3)'></form>";
  html += "</div>";

  html += "<div class='panel'>";
  html += "<h3>Firmware Update (OTA)</h3>";
  html += "<div class='note'>Chọn file firmware.bin (build từ PlatformIO: .pio/build/esp32-s3-devkitc-1/firmware.bin) rồi bấm Upload. Board tự khởi động lại sau khi nạp xong. KHÔNG rút nguồn/mất mạng giữa chừng - có thể phải nạp lại qua USB nếu hỏng.</div>";
  html += "<form action='/update' method='POST' enctype='multipart/form-data' onsubmit=\"return confirm('Nạp firmware mới? Board sẽ khởi động lại sau khi xong.');\">";
  html += "<input type='file' name='firmware' accept='.bin' required style='width:100%;padding:10px;border:1px solid #bfc9d6;border-radius:8px;margin-bottom:8px;background:#fff'>";
  html += "<input class='btn' type='submit' value='Upload & Update'>";
  html += "</form>";
  html += "</div>";

  html += "</div></body></html>";

  server.send(200, "text/html", html);
}
