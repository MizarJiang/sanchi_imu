#include <deque>
#include <ros/ros.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/MagneticField.h>
#include <sensor_msgs/NavSatFix.h>
#include <tf/tf.h>
#include <eigen3/Eigen/Geometry> 
#include <chrono>
#include <locale>
#include <tuple>
#include <algorithm>
#include <iostream>
#include <string>
#include <sstream>
#include <stdexcept>
#include <boost/assert.hpp>
#include <boost/asio.hpp>
#include <boost/asio/serial_port.hpp>

extern "C" {
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <unistd.h> //  close
#include <string.h> //  strerror
}


using namespace std;

static int data_length = 81;


boost::asio::serial_port* serial_port = 0;
static const uint8_t stop[6] = {0xA5, 0x5A, 0x04, 0x02, 0x06, 0xAA};
static const uint8_t mode[6] = {0xA5, 0x5A, 0x04, 0x01, 0x05, 0xAA};
static uint8_t data_raw[200];
static std::vector<uint8_t> buffer_;
static std::deque<uint8_t> queue_;
static std::string name, frame_id;
static sensor_msgs::Imu msg;
static sensor_msgs::MagneticField msg_mag;
static sensor_msgs::NavSatFix msg_gps;
static int fd_ = -1;
static ros::Publisher pub, pub_mag, pub_gps;
static uint8_t tmp[81];

static float d2f_acc(uint8_t a[2])
{
    int16_t acc = a[0];
    acc = (acc << 8) | a[1];
    return ((float) acc) / 16384.0f;
}

static float d2f_gyro(uint8_t a[2])
{
    int16_t acc = a[0];
    acc = (acc << 8) | a[1];
    return ((float) acc) / 32.8f;
}

static float d2f_mag(uint8_t a[2])
{
    int16_t acc = a[1];
    acc = (acc << 8) | a[0];
    return ((float) acc) / 1.0f;
}

static float d2f_euler(uint8_t a[2])
{
    int16_t acc = a[0];
    acc = (acc << 8) | a[1];
    return ((float) acc) / 10.0f;
}


static double d2f_latlon(uint8_t a[4])
{
    int64_t high = a[0];
    high = (high << 8) | a[1];

    int64_t low = a[2];
    low = (low << 8) | a[3];
    return (double)((high << 8) | low);
}

static double d2f_gpsvel(uint8_t a[2])
{
    int16_t acc = a[0];
    acc = (acc << 8) | a[1];
    return ((float) acc) / 10.0f;
}

static float d2ieee754(uint8_t a[4])
{
    union fnum {
        float f_val;
        uint8_t d_val[4];
    } f;

    memcpy(f.d_val, a, 4);
    return f.f_val;
}

static double d2_32f(uint8_t a[4])
{
    union fnum {
        float f_val;
        uint8_t d_val[4];
    } f;

    memcpy(f.d_val, a, 4);
    return f.f_val;
}

static double d2_64f(uint8_t a[8])
{
    union fnum {
        double f_val;
        uint8_t d_val[8];
    } f;

    memcpy(f.d_val, a, 8);
    return f.f_val;
}

int uart_set(int fd, int baude, int c_flow, int bits, char parity, int stop)
{
    struct termios options;
 
    if(tcgetattr(fd, &options) < 0)
    {
        perror("tcgetattr error");
        return -1;
    }

    cfsetispeed(&options,B115200);
    cfsetospeed(&options,B115200);

    options.c_cflag |= CLOCAL;
    options.c_cflag |= CREAD;

    switch(c_flow)
    {
        case 0:
            options.c_cflag &= ~CRTSCTS;
            break;
        case 1:
            options.c_cflag |= CRTSCTS;
            break;
        case 2:
            options.c_cflag |= IXON|IXOFF|IXANY;
            break;
        default:
            fprintf(stderr,"Unkown c_flow!\n");
            return -1;
    }

    switch(bits)
    {
        case 5:
            options.c_cflag &= ~CSIZE;
            options.c_cflag |= CS5;
            break;
        case 6:
            options.c_cflag &= ~CSIZE;
            options.c_cflag |= CS6;
            break;
        case 7:
            options.c_cflag &= ~CSIZE;
            options.c_cflag |= CS7;
            break;
        case 8:
            options.c_cflag &= ~CSIZE;
            options.c_cflag |= CS8;
            break;
        default:
            fprintf(stderr,"Unkown bits!\n");
            return -1;
    }

    switch(parity)
    {
        case 'n':
        case 'N':
            options.c_cflag &= ~PARENB;
            options.c_cflag &= ~INPCK;
            break;

        case 's':
        case 'S':
            options.c_cflag &= ~PARENB;
            options.c_cflag &= ~CSTOPB;
            break;

        case 'o':
        case 'O':
            options.c_cflag |= PARENB;
            options.c_cflag |= PARODD;
            options.c_cflag |= INPCK;
            options.c_cflag |= ISTRIP;
            break;

        case 'e':
        case 'E':
            options.c_cflag |= PARENB;
            options.c_cflag &= ~PARODD;
            options.c_cflag |= INPCK;
            options.c_cflag |= ISTRIP;
            break;
        default:
            fprintf(stderr,"Unkown parity!\n");
            return -1;
    }

    switch(stop)
    {
        case 1:
            options.c_cflag &= ~CSTOPB;
            break;
        case 2:
            options.c_cflag |= CSTOPB;
            break;
        default:
            fprintf(stderr,"Unkown stop!\n");
            return -1;
    }

    options.c_oflag &= ~OPOST;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_cc[VTIME] = 0;
    options.c_cc[VMIN] = 1;

    tcflush(fd,TCIFLUSH);

    if(tcsetattr(fd,TCSANOW,&options) < 0)
    {
        perror("tcsetattr failed");
        return -1;
    }

    return 0;

}


int main(int argc, char** argv)
{
  ros::init(argc, argv, "imu");
  ros::NodeHandle n("~");

  name = ros::this_node::getName();

  std::string port;
  if (n.hasParam("port"))
    n.getParam("port", port);
  else
    {
      ROS_ERROR("%s: must provide a port", name.c_str());
      return -1;
    }

  std::string model;
  if (n.hasParam("model"))
    n.getParam("model", model);
  else
    {
      ROS_ERROR("%s: must provide a model name", name.c_str());
      return -1;
    }


  ROS_WARN("Model set to %s", model.c_str());

  int baud;
  if (n.hasParam("baud"))
    n.getParam("baud", baud);
  else
    {
      ROS_ERROR("%s: must provide a baudrate", name.c_str());
      return -1;
    }



  ROS_WARN("Baudrate set to %d", baud);
  
  n.param("frame_id", frame_id, string("world"));

  double delay;
  n.param("delay", delay, 0.0);

  boost::asio::io_service io_service;
  serial_port = new boost::asio::serial_port(io_service);
  try
    {
      serial_port->open(port);
    }
  catch (boost::system::system_error &error)
    {
      ROS_ERROR("%s: Failed to open port %s with error %s",
                name.c_str(), port.c_str(), error.what());
      return -1;
    }

  if (!serial_port->is_open())
    {
      ROS_ERROR("%s: failed to open serial port %s",
                name.c_str(), port.c_str());
      return -1;
  }

  typedef boost::asio::serial_port_base sb;

  sb::baud_rate baud_option(baud);
  sb::flow_control flow_control(sb::flow_control::none);
  sb::parity parity(sb::parity::none);
  sb::stop_bits stop_bits(sb::stop_bits::one);

  serial_port->set_option(baud_option);
  serial_port->set_option(flow_control);
  serial_port->set_option(parity);
  serial_port->set_option(stop_bits);

  const char *path = port.c_str();
  fd_ = open(path, O_RDWR);
  if(fd_ < 0)
  {    
      ROS_ERROR("Port Error!: %s", path);
      return -1;
  }
  
  pub = n.advertise<sensor_msgs::Imu>("data_raw", 1);
  pub_mag = n.advertise<sensor_msgs::MagneticField>("mag", 1);
  pub_gps = n.advertise<sensor_msgs::NavSatFix>("gps", 1);

  if(model == "100S")
  {
      write(fd_, stop, 6);
      usleep(1000 * 1000);
      write(fd_, mode, 6);
      usleep(1000 * 1000);
      data_length = 81;
  }
  else if(model == "200A")
  {
      uint8_t speed[7] = {0xA5, 0x5A, 0x05, 0xA8, 0x64, 0x11, 0xaa};
      write(fd_, speed, 7);
      usleep(1000 * 1000);
      data_length = 61;
  }
  else if(model == "300A")
  {
      uint8_t speed[7] = {0xA5, 0x5A, 0x05, 0xA8, 0x64, 0x11, 0xaa};
      write(fd_, speed, 7);
      usleep(1000 * 1000);
      data_length = 61;
  }
  else if(model == "200S")
  {
      uint8_t speed[7] = {0xA5, 0x5A, 0x05, 0xA8, 0x0a, 0xb7, 0xaa};//10HZ
    //   uint8_t speed[7] = {0xA5, 0x5A, 0x05, 0xA8, 0x14, 0xc1, 0xaa};//20HZ
    //   uint8_t speed[7] = {0xA5, 0x5A, 0x05, 0xA8, 0x1e, 0xcb, 0xaa};//30HZ
    //   uint8_t speed[7] = {0xA5, 0x5A, 0x05, 0xA8, 0x28, 0xd5, 0xaa};//40HZ
    //   uint8_t speed[7] = {0xA5, 0x5A, 0x05, 0xA8, 0x32, 0xdf, 0xaa};//50HZ
    //   uint8_t speed[7] = {0xA5, 0x5A, 0x05, 0xA8, 0x3c, 0xe9, 0xaa};//60HZ
    //   uint8_t speed[7] = {0xA5, 0x5A, 0x05, 0xA8, 0x46, 0xf3, 0xaa};//70HZ
    //   uint8_t speed[7] = {0xA5, 0x5A, 0x05, 0xA8, 0x50, 0xfd, 0xaa};//80HZ
    //   uint8_t speed[7] = {0xA5, 0x5A, 0x05, 0xA8, 0x5a, 0x07, 0xaa};//90HZ
    //   uint8_t speed[7] = {0xA5, 0x5A, 0x05, 0xA8, 0x64, 0x11, 0xaa};//100HZ
      write(fd_, speed, 7);
      usleep(1000 * 1000);
      data_length = 92;
  }
  if(model == "100D2")
   {
       write(fd_, stop, 6);
       usleep(1000 * 1000);
       write(fd_, mode, 6);
       usleep(1000 * 1000);
       data_length = 40;
   }

  int kk = 0;
  ROS_WARN("Streaming Data...");
  while (n.ok())
  {
      read(fd_, tmp, sizeof(uint8_t) * data_length);
      memcpy(data_raw, tmp, sizeof(uint8_t) * data_length);

      bool found = false;
      for(kk = 0; kk < data_length - 1; ++kk)
      {
	  if(model == "100S" && data_raw[kk] == 0xA5 && data_raw[kk + 1] == 0x5A)
          {
              unsigned char *data = data_raw + kk;
	      if(((data[2] != 0x14 && data[3] != 0xA1) && 
                  (data[2] != 0x16 && data[3] != 0xA2) &&
                  (data[2] != 0x14 && data[3] != 0xA3) &&
                  (data[2] != 0x13 && data[3] != 0xA6)) || (data_length - kk) < 13)
		  continue;
              uint8_t len = data[2];

              uint32_t checksum = 0;
              for(int i = 0; i < len; ++i)
                  checksum += (uint32_t) data[i];
  
              uint16_t check = checksum % 256 + 1;
              uint16_t check_true = data[len];
              if (check != check_true)
              {
                  continue;
              }

	      if(data[3] == 0xA1)
              {   
                  Eigen::Vector3d ea0(d2f_euler(data + 4) * M_PI / 180.0,
                                      d2f_euler(data + 6) * M_PI / 180.0,
                                      d2f_euler(data + 8) * M_PI / 180.0);  
                  Eigen::Matrix3d R;  
                  R = Eigen::AngleAxisd(ea0[0], ::Eigen::Vector3d::UnitZ())  
                      * Eigen::AngleAxisd(ea0[1], ::Eigen::Vector3d::UnitY())  
                      * Eigen::AngleAxisd(ea0[2], ::Eigen::Vector3d::UnitX());  
                  Eigen::Quaterniond q;   
                  q = R; 
                  msg.orientation.w = (double)q.w();
                  msg.orientation.x = (double)q.x();
                  msg.orientation.y = (double)q.y();
                  msg.orientation.z = (double)q.z();    
              }
              else if(data[3] == 0xA2)
	      {
                  
                  msg.header.stamp = ros::Time::now();
                  msg.header.frame_id = frame_id;
                  msg.angular_velocity.x = d2f_gyro(data + 10);
          	  msg.angular_velocity.y = d2f_gyro(data + 12);
          	  msg.angular_velocity.z = d2f_gyro(data + 14);
                  msg.linear_acceleration.x = d2f_acc(data + 4) * 9.81;
                  msg.linear_acceleration.y = d2f_acc(data + 6) * 9.81;
                  msg.linear_acceleration.z = d2f_acc(data + 8) * 9.81;
                  pub.publish(msg);

                  msg_mag.magnetic_field.x = d2f_mag(data + 16);
                  msg_mag.magnetic_field.y = d2f_mag(data + 18);
                  msg_mag.magnetic_field.z = d2f_mag(data + 20);
                  msg_mag.header.stamp = msg.header.stamp;
                  msg_mag.header.frame_id = msg.header.frame_id;
                  pub_mag.publish(msg_mag);
              }

              else if(data[3] == 0xA6)
              {
                  msg_gps.header.stamp = ros::Time::now();
                  msg_gps.header.frame_id = frame_id;
                  msg_gps.latitude = d2f_latlon(data + 4) * 1e-6;
                  msg_gps.longitude = d2f_latlon(data + 8) * 1e-6;
                  
                  if(data[18] == 0x22)
                  {
                      msg_gps.latitude = msg_gps.latitude;
                      msg_gps.longitude = msg_gps.longitude;
                  }
                  else if(data[18] == 0x12)
                  {
                      msg_gps.latitude = - msg_gps.latitude;
                      msg_gps.longitude = msg_gps.longitude;
                  }
                  else if(data[18] == 0x11)
                  {
                      msg_gps.latitude = - msg_gps.latitude;
                      msg_gps.longitude = - msg_gps.longitude;
                  }
                  else if(data[18] == 0x21)
                  {
                      msg_gps.latitude = msg_gps.latitude;
                      msg_gps.longitude = - msg_gps.longitude;
                  }

                  msg_gps.altitude = d2f_latlon(data + 16) / 10.0f;

                  pub_gps.publish(msg_gps);
              }

              found = true;
          }
	  else if(model == "200A" && data_raw[kk] == 0x55 && data_raw[kk + 1] == 0xAA && data_raw[kk + data_length - 1] == 0xBB )
          {
              unsigned char *data = data_raw + kk;
              uint32_t checksum = 0;
              for(int i = 2; i < data_length - 2; ++i)
                  checksum += (uint32_t) data[i];
  
              uint16_t check = checksum % 256;
              uint16_t check_true = data[data_length - 2];
              if (check != check_true)
              {
                  continue;
              }

                  Eigen::Vector3d ea0(d2ieee754(data + 39) * M_PI / 180.0,
                                      d2ieee754(data + 43) * M_PI / 180.0,
                                      d2ieee754(data + 47) * M_PI / 180.0);  
                  Eigen::Matrix3d R;
                  R = Eigen::AngleAxisd(ea0[0], ::Eigen::Vector3d::UnitZ())  
                      * Eigen::AngleAxisd(ea0[1], ::Eigen::Vector3d::UnitY())  
                      * Eigen::AngleAxisd(ea0[2], ::Eigen::Vector3d::UnitX());  
                  Eigen::Quaterniond q;   
                  q = R; 
                  msg.orientation.w = (double)q.w();
                  msg.orientation.x = (double)q.x();
                  msg.orientation.y = (double)q.y();
                  msg.orientation.z = (double)q.z();    
                  
                  msg.header.stamp = ros::Time::now();
                  msg.header.frame_id = frame_id;
                  msg.angular_velocity.x = d2ieee754(data + 15) * M_PI /180;
          	      msg.angular_velocity.y = d2ieee754(data + 19) * M_PI /180;
          	      msg.angular_velocity.z = d2ieee754(data + 23) * M_PI /180;
                  msg.linear_acceleration.x = d2ieee754(data + 3) * 1e-3 * 9.81;
                  msg.linear_acceleration.y = d2ieee754(data + 7) * 1e-3 * 9.81;
                  msg.linear_acceleration.z = d2ieee754(data + 11) * 1e-3 * 9.81;
                  pub.publish(msg);

                  msg_mag.magnetic_field.x = d2ieee754(data + 27);
                  msg_mag.magnetic_field.y = d2ieee754(data + 31);
                  msg_mag.magnetic_field.z = d2ieee754(data + 35);
                  msg_mag.header.stamp = msg.header.stamp;
                  msg_mag.header.frame_id = msg.header.frame_id;
                  pub_mag.publish(msg_mag);

              found = true;
          }
		  else if(model == "300A" && data_raw[kk] == 0x55 && data_raw[kk + 1] == 0xAA && data_raw[kk + data_length - 1] == 0xBB )
          {
              unsigned char *data = data_raw + kk;
              uint32_t checksum = 0;
              for(int i = 2; i < data_length - 2; ++i)
                  checksum += (uint32_t) data[i];
  
              uint16_t check = checksum % 256;
              uint16_t check_true = data[data_length - 2];
              if (check != check_true)
              {
                  continue;
              }

                  Eigen::Vector3d ea0(d2ieee754(data + 39) * M_PI / 180.0,
                                      d2ieee754(data + 43) * M_PI / 180.0,
                                      d2ieee754(data + 47) * M_PI / 180.0);  
                  Eigen::Matrix3d R;
                  R = Eigen::AngleAxisd(ea0[0], ::Eigen::Vector3d::UnitY())  
                      * Eigen::AngleAxisd(-ea0[1], ::Eigen::Vector3d::UnitX())  
                      * Eigen::AngleAxisd(-ea0[2], ::Eigen::Vector3d::UnitZ());  
                  Eigen::Quaterniond q;   
                  q = R; 
                  msg.orientation.w = (double)q.w();
                  msg.orientation.x = (double)q.x();
                  msg.orientation.y = (double)q.y();
                  msg.orientation.z = (double)q.z();    
                  
                  msg.header.stamp = ros::Time::now();
                  msg.header.frame_id = frame_id;
                  msg.angular_velocity.x = d2ieee754(data + 15) * M_PI /180;
          	      msg.angular_velocity.y = d2ieee754(data + 19) * M_PI /180;
          	      msg.angular_velocity.z = d2ieee754(data + 23) * M_PI /180;
                  msg.linear_acceleration.x = d2ieee754(data + 3) * 1e-3 * 9.81;
                  msg.linear_acceleration.y = d2ieee754(data + 7) * 1e-3 * 9.81;
                  msg.linear_acceleration.z = d2ieee754(data + 11) * 1e-3 * 9.81;
                  pub.publish(msg);

                  msg_mag.magnetic_field.x = d2ieee754(data + 27);
                  msg_mag.magnetic_field.y = d2ieee754(data + 31);
                  msg_mag.magnetic_field.z = d2ieee754(data + 35);
                  msg_mag.header.stamp = msg.header.stamp;
                  msg_mag.header.frame_id = msg.header.frame_id;
                  pub_mag.publish(msg_mag);

              found = true;
          }
          else if(model == "200S" && data_raw[kk] == 0x55 && data_raw[kk + 1] == 0xAA && data_raw[kk + data_length - 1] == 0xBB )
          {
              unsigned char *data = data_raw + kk;
              uint32_t checksum = 0;
              for(int i = 2; i < data_length - 2; ++i)
                  checksum += (uint32_t) data[i];

              uint16_t check = checksum % 256;
              uint16_t check_true = data[data_length - 2];
              if (check != check_true)
              {
                  continue;
              }

                // 代码读取顺序：yaw,pitch,roll ，通讯协议顺序为：pitch,roll,yaw
                  Eigen::Vector3d ea0(-d2ieee754(data + 25) * M_PI / 180.0,
                                      -d2ieee754(data + 17) * M_PI / 180.0,
                                      d2ieee754(data + 21) * M_PI / 180.0);  
                  Eigen::Matrix3d R;  
                  R = Eigen::AngleAxisd(ea0[0], ::Eigen::Vector3d::UnitZ())  
                      * Eigen::AngleAxisd(ea0[1], ::Eigen::Vector3d::UnitY())  
                      * Eigen::AngleAxisd(ea0[2], ::Eigen::Vector3d::UnitX());  
                  Eigen::Quaterniond q;   
                  q = R; 
                  msg.orientation.w = (double)q.w();
                  msg.orientation.x = (double)q.x();
                  msg.orientation.y = (double)q.y();
                  msg.orientation.z = (double)q.z();    
                  
                  msg.header.stamp = ros::Time::now();
                  msg.header.frame_id = frame_id;
                  msg.angular_velocity.y = -d2f_mag(data + 9) * 0.02 * M_PI /180;
          	      msg.angular_velocity.x = d2f_mag(data + 11) * 0.02 * M_PI /180;
          	      msg.angular_velocity.z = d2f_mag(data + 13) * 0.02 * M_PI /180;

                //   int16_t it_temputure = d2f_mag(data + 15);
                //   std::cout << "程序传感器温度" <<(float)it_temputure<< std::endl;

                  msg.linear_acceleration.y = -d2f_mag(data + 3) * 0.5 * 1e-3 * 9.81;
                  msg.linear_acceleration.x = d2f_mag(data + 5) * 0.5 * 1e-3 * 9.81;
                  msg.linear_acceleration.z = d2f_mag(data + 7) * 0.5 * 1e-3 * 9.81;

                  pub.publish(msg);

                  msg_mag.magnetic_field.x = d2f_mag(data + 70) * 0.003;
                  msg_mag.magnetic_field.y = d2f_mag(data + 72) * 0.003;
                  msg_mag.magnetic_field.z = d2f_mag(data + 74) * 0.003;
                  msg_mag.header.stamp = msg.header.stamp;
                  msg_mag.header.frame_id = msg.header.frame_id;

                  pub_mag.publish(msg_mag);

                  msg_gps.header.stamp = ros::Time::now();
                  msg_gps.header.frame_id = frame_id;
                  msg_gps.latitude = d2_64f(data + 43);
                  msg_gps.longitude = d2_64f(data + 35);
                  msg_gps.altitude = d2_32f(data + 51);
                //   ROS_INFO("latitude = %f",msg_gps.latitude);
                //   ROS_INFO("longitude = %f",msg_gps.longitude);
                //   ROS_INFO("altitude = %f",msg_gps.altitude);

                  pub_gps.publish(msg_gps);
		

              found = true;
          }
          if(model == "100D2" && data_raw[kk] == 0xA5 && data_raw[kk + 1] == 0x5A)
          {
              unsigned char *data = data_raw + kk;

              uint8_t data_length = data[2];

              uint32_t checksum = 0;
              for(int i = 0; i < data_length - 1  ; ++i)
              {
                  checksum += (uint32_t) data[i+2];
              }

              uint16_t check = checksum % 256;
              uint16_t check_true = data[data_length+1];

              if (check != check_true)
              {
                  printf("check error\n");
                  continue;
              }

              Eigen::Vector3d ea0(-d2f_euler(data + 3) * M_PI / 180.0,
                                  d2f_euler(data + 7) * M_PI / 180.0,
                                  d2f_euler(data + 5) * M_PI / 180.0);
              Eigen::Matrix3d R;
              R = Eigen::AngleAxisd(ea0[0], ::Eigen::Vector3d::UnitZ())
                  * Eigen::AngleAxisd(ea0[1], ::Eigen::Vector3d::UnitY())
                  * Eigen::AngleAxisd(ea0[2], ::Eigen::Vector3d::UnitX());
              Eigen::Quaterniond q;
              q = R;
              msg.orientation.w = (double)q.w();
              msg.orientation.x = (double)q.x();
              msg.orientation.y = (double)q.y();
              msg.orientation.z = (double)q.z();

              msg.header.stamp = ros::Time::now();
              msg.header.frame_id = frame_id;
              msg.angular_velocity.x = d2f_gyro(data + 15);
              msg.angular_velocity.y = d2f_gyro(data + 17);
              msg.angular_velocity.z = d2f_gyro(data + 19);
              msg.linear_acceleration.x = d2f_acc(data + 9) * 9.81;
              msg.linear_acceleration.y = d2f_acc(data + 11) * 9.81;
              msg.linear_acceleration.z = d2f_acc(data + 13) * 9.81;
              pub.publish(msg);

              msg_mag.magnetic_field.x = d2f_mag(data + 21);
              msg_mag.magnetic_field.y = d2f_mag(data + 23);
              msg_mag.magnetic_field.z = d2f_mag(data + 25);
              msg_mag.header.stamp = msg.header.stamp;
              msg_mag.header.frame_id = msg.header.frame_id;
              pub_mag.publish(msg_mag);

              found = true;
          }
      }
  }

  // Stop continous and close device
  ROS_WARN("Wait 0.1s"); 
  ros::Duration(0.1).sleep();
  ::close(fd_);

  return 0;
}
