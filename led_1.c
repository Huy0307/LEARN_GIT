#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>       // Cần thiết cho các chức năng GPIO
#include <linux/kobject.h>    // Sử dụng kobjects cho việc kết nối sysfs
#include <linux/kthread.h>    // Sử dụng kthreads cho chức năng nhấp nháy
#include <linux/delay.h>      // Sử dụng header này cho hàm msleep()

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LED_TEST");
MODULE_DESCRIPTION("A simple Linux LED driver LKM for the AM62x");
MODULE_VERSION("0.1");

static unsigned int gpioLED = 504;           ///< GPIO mặc định cho LED là 504
module_param(gpioLED, uint, S_IRUGO);       ///< Tham số mô tả. S_IRUGO có thể đọc/không thay đổi
MODULE_PARM_DESC(gpioLED, "GPIO LED number (default=504)");     ///< mô tả tham số

static unsigned int blinkPeriod = 1000;     ///< Chu kỳ nhấp nháy trong ms
module_param(blinkPeriod, uint, S_IRUGO);   ///< Tham số mô tả. S_IRUGO có thể đọc/không thay đổi
MODULE_PARM_DESC(blinkPeriod, " LED blink period in ms (min=1, default=1000, max=10000)");

static char ledName[7] = "ledXXX";          ///< Chuỗi mặc định kết thúc bằng null -- phòng trường hợp không mong muốn
static bool ledOn = 0;                      ///< Đèn LED đang bật hay tắt? Được sử dụng cho chức năng nhấp nháy
enum modes { OFF, ON, FLASH };              ///< Các chế độ LED có sẵn -- static không hữu ích ở đây
static enum modes mode = FLASH;             ///< Chế độ mặc định là nhấp nháy

/** @brief Hàm callback để hiển thị chế độ LED
 *  @param kobj đại diện cho một thiết bị đối tượng kernel xuất hiện trong hệ thống tệp sysfs
 *  @param attr con trỏ đến cấu trúc kobj_attribute
 *  @param buf bộ đệm để ghi số lần nhấn
 *  @return trả về số ký tự của chuỗi chế độ được hiển thị thành công
 */
static ssize_t mode_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
   switch(mode){
      case OFF:   return sprintf(buf, "off\n");       // Hiển thị trạng thái -- cách tiếp cận đơn giản
      case ON:    return sprintf(buf, "on\n");
      case FLASH: return sprintf(buf, "flash\n");
      default:    return sprintf(buf, "LKM Error\n"); // Không thể đến đây được
   }
}

/** @brief Hàm callback để lưu chế độ LED sử dụng enum ở trên */
static ssize_t mode_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
   // count-1 quan trọng vì ngược lại \n sẽ được sử dụng trong so sánh
   if (strncmp(buf,"on",count-1)==0) { mode = ON; }   // strncmp() so sánh với số ký tự cố định
   else if (strncmp(buf,"off",count-1)==0) { mode = OFF; }
   else if (strncmp(buf,"flash",count-1)==0) { mode = FLASH; }
   return count;
}

/** @brief Hàm callback để hiển thị chu kỳ LED */
static ssize_t period_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
   return sprintf(buf, "%d\n", blinkPeriod);
}

/** @brief Hàm callback để lưu giá trị chu kỳ LED */
static ssize_t period_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
   unsigned int period;                     // Sử dụng biến để xác minh dữ liệu được gửi
   sscanf(buf, "%du", &period);             // Đọc chu kỳ như một unsigned int
   if ((period>1)&&(period<=10000)){        // Phải là 2ms hoặc lớn hơn, 10 giây hoặc ít hơn
      blinkPeriod = period;                 // Trong phạm vi, gán vào biến blinkPeriod
   }
   return period;
}

/** Sử dụng các macro helper này để định nghĩa tên và cấp độ truy cập của kobj_attributes
 *  kobj_attribute có thuộc tính attr (tên và mode), con trỏ hàm show và store
 *  Biến chu kỳ liên kết với biến blinkPeriod và nó được tiết lộ
 *  với mode 0666 sử dụng các hàm period_show và period_store ở trên
 */
static struct kobj_attribute period_attr = __ATTR(blinkPeriod, 0664, period_show, period_store);
static struct kobj_attribute mode_attr = __ATTR(mode, 0664, mode_show, mode_store);

/** ebb_attrs[] là một mảng các thuộc tính được sử dụng để tạo ra nhóm thuộc tính ở dưới.
 *  Thuộc tính attr của kobj_attribute được sử dụng để trích xuất cấu trúc thuộc tính
 */
static struct attribute *ebb_attrs[] = {
   &period_attr.attr,                       // Chu kỳ mà LED nhấp nháy
   &mode_attr.attr,                         // LED đang bật hay tắt?
   NULL,
};

/** Nhóm thuộc tính sử dụng mảng thuộc tính và tên, được tiết lộ trên sysfs -- trong trường hợp này là gpio504,
 *  nó được tự động định nghĩa trong hàm ebbLED_init() dưới đây
 *  sử dụng tham số nhân kernel tùy chỉnh có thể được truyền khi module được tải.
 */
static struct attribute_group attr_group = {
   .name  = ledName,                        // Tên được tạo ra trong ebbLED_init()
   .attrs = ebb_attrs,                      // Mảng thuộc tính được định nghĩa ngay trên
};

static struct kobject *ebb_kobj;            /// Con trỏ đến kobject
static struct task_struct *task;            /// Con trỏ đến nhiệm vụ luồng

/** @brief Vòng lặp chính của LED Flasher kthread
 *
 *  @param arg Một con trỏ void được sử dụng để chuyển dữ liệu cho luồng
 *  @return trả về 0 nếu thành công
 */
static int flash(void *arg){
   printk(KERN_INFO "EBB LED: Thread has started running \n");
   while(!kthread_should_stop()){           // Trả về true khi gọi kthread_stop()
      set_current_state(TASK_RUNNING);
      if (mode==FLASH) ledOn = !ledOn;      // Đảo trạng thái LED
      else if (mode==ON) ledOn = true;
      else ledOn = false;
      gpio_set_value(gpioLED, ledOn);       // Sử dụng trạng thái LED để bật/tắt LED
      set_current_state(TASK_INTERRUPTIBLE);
      msleep(blinkPeriod/2);                // sleep theo miligiây cho một nửa chu kỳ
   }
   printk(KERN_INFO "EBB LED: Thread has run to completion \n");
   return 0;
}

/** @brief Hàm khởi tạo LKM
 *  keyword static giới hạn việc nhìn thấy của hàm trong file C này. Macro __init
 *  có nghĩa là đối với driver tích hợp (không phải là LKM) hàm này chỉ được sử dụng vào thời gian khởi tạo
 *  và nó có thể được loại bỏ và bộ nhớ của nó được giải phóng sau thời điểm đó. Trong ví dụ này
 *  hàm này thiết lập các GPIO và IRQ
 *  @return trả về 0 nếu thành công
 */
static int __init ebbLED_init(void){
   int result = 0;

   printk(KERN_INFO "EBB LED: Initializing the EBB LED LKM\n");
   sprintf(ledName, "led%d", gpioLED);      // Tạo tên gpio115 cho /sys/ebb/led504

   ebb_kobj = kobject_create_and_add("ebb", kernel_kobj->parent); // kernel_kobj trỏ đến /sys/kernel
   if(!ebb_kobj){
      printk(KERN_ALERT "EBB LED: failed to create kobject\n");
      return -ENOMEM;
   }
   // thêm các thuộc tính vào /sys/ebb/ -- ví dụ: /sys/ebb/led504/ledOn
   result = sysfs_create_group(ebb_kobj, &attr_group);
   if(result) {
      printk(KERN_ALERT "EBB LED: failed to create sysfs group\n");
      kobject_put(ebb_kobj);                // dọn dẹp -- xóa bỏ mục kobject sysfs
      return result;
   }
   ledOn = true;
   gpio_request(gpioLED, "sysfs");          // gpioLED là 504 mặc định, yêu cầu nó
   gpio_direction_output(gpioLED, ledOn);   // Đặt gpio ở chế độ output và bật
   gpio_export(gpioLED, false);  // khiến gpio504 xuất hiện trong /sys/class/gpio
                                 // đối số thứ hai ngăn việc thay đổi hướng

   task = kthread_run(flash, NULL, "LED_flash_thread");  // Bắt đầu luồng nhấp nháy LED
   if(IS_ERR(task)){                                     // Tên Kthread là LED_flash_thread
      printk(KERN_ALERT "EBB LED: failed to create the task\n");
      return PTR_ERR(task);
   }
   return result;
}

/** @brief Hàm dọn dẹp LKM
 *  Tương tự như hàm khởi tạo, nó là static. Macro __exit thông báo rằng nếu mã này được sử dụng cho driver tích hợp
 *  (không phải LKM) thì hàm này không cần thiết.
 */
static void __exit ebbLED_exit(void){
   kthread_stop(task);                      // Dừng luồng nhấp nháy LED
   kobject_put(ebb_kobj);                   // dọn dẹp -- xóa bỏ mục kobject sysfs
   gpio_set_value(gpioLED, 0);              // Tắt LED, chỉ ra rằng thiết bị đã bị gỡ bỏ
   gpio_unexport(gpioLED);                  // Không xuất GPIO nữa
   gpio_free(gpioLED);                      // Giải phóng GPIO LED
   printk(KERN_INFO "EBB LED: Goodbye from the EBB LED LKM!\n");
}

/// Những cuộc gọi tiếp theo là bắt buộc -- chúng xác định hàm khởi tạo
/// và hàm dọn dẹp (như trên).
module_init(ebbLED_init);
module_exit(ebbLED_exit);
