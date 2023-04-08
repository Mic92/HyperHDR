// LedDevice includes
#include <leddevice/LedDevice.h>
#include "ProviderFtdi.h"

#include <ftdi.h>
#include <libusb.h>

#include <QEventLoop>

#define ANY_FTDI_VENDOR 0x0
#define ANY_FTDI_PRODUCT 0x0

namespace Pin
{
	// enumerate the AD bus for conveniance.
	enum bus_t
	{
		SK = 0x01, // ADBUS0, SPI data clock
		DO = 0x02, // ADBUS1, SPI data out
		CS = 0x08, // ADBUS3, SPI chip select, active low
	};
}

const unsigned char pinInitialState = Pin::CS;
// Use these pins as outputs
const unsigned char pinDirection = Pin::SK | Pin::DO | Pin::CS;

const QString ProviderFtdi::AUTO_SETTING = QString("auto");

ProviderFtdi::ProviderFtdi(const QJsonObject &deviceConfig)
	: LedDevice(deviceConfig),
	  _ftdic(nullptr),
	  _baudRate_Hz(1000000)
{
}

bool ProviderFtdi::init(const QJsonObject &deviceConfig)
{
	bool isInitOK = false;

	if (LedDevice::init(deviceConfig))
	{
		_baudRate_Hz = deviceConfig["rate"].toInt(_baudRate_Hz);
		_deviceName = deviceConfig["output"].toString(AUTO_SETTING);

		Debug(_log, "_baudRate_Hz [%d]", _baudRate_Hz);
		Debug(_log, "_deviceName [%s]", QSTRING_CSTR(_deviceName));

		isInitOK = true;
	}
	return isInitOK;
}

int ProviderFtdi::openDevice()
{
	_ftdic = ftdi_new();

	bool autoDiscovery = (QString::compare(_deviceName, ProviderFtdi::AUTO_SETTING, Qt::CaseInsensitive) == 0);
	Debug(_log, "Opening FTDI device=%s autoDiscovery=%s", QSTRING_CSTR(_deviceName), autoDiscovery ? "true" : "false");
	if (autoDiscovery)
	{
		struct ftdi_device_list *devlist;
		int devicesDetected = 0;
		if ((devicesDetected = ftdi_usb_find_all(_ftdic, &devlist, ANY_FTDI_VENDOR, ANY_FTDI_PRODUCT)) < 0)
		{
			setInError(ftdi_get_error_string(_ftdic));
			return -1;
		}
		if (devicesDetected == 0)
		{
			setInError("No ftdi devices detected");
			return 0;
		}

		if (ftdi_usb_open_dev(_ftdic, devlist[0].dev) < 0)
		{
			setInError(ftdi_get_error_string(_ftdic));
			ftdi_list_free(&devlist);
			return -1;
		}

		ftdi_list_free(&devlist);
		return 1;
	}
	else
	{
		if (ftdi_usb_open_string(_ftdic, QSTRING_CSTR(_deviceName)) < 0)
		{
			setInError(ftdi_get_error_string(_ftdic));
			return -1;
		}
		return 1;
	}
}
int ProviderFtdi::open()
{
	int rc = 0;

	if ((rc = openDevice()) != 1)
	{
		return -1;
	}

	/* doing this disable resets things if they were in a bad state */
	if ((rc = ftdi_disable_bitbang(_ftdic)) < 0)
	{
		setInError(ftdi_get_error_string(_ftdic));
		return rc;
	}
	if ((rc = ftdi_setflowctrl(_ftdic, SIO_DISABLE_FLOW_CTRL)) < 0)
	{
		setInError(ftdi_get_error_string(_ftdic));
		return rc;
	}
	if ((rc = ftdi_set_bitmode(_ftdic, 0x00, BITMODE_RESET)) < 0)
	{
		setInError(ftdi_get_error_string(_ftdic));
		return rc;
	}

	if ((rc = ftdi_set_bitmode(_ftdic, 0xff, BITMODE_MPSSE)) < 0)
	{
		setInError(ftdi_get_error_string(_ftdic));
		return rc;
	}

	double reference_clock = 60e6;
	int divisor = (reference_clock / 2 / _baudRate_Hz) - 1;
	uint8_t buf[10] = {0};
	unsigned int icmd = 0;
	buf[icmd++] = DIS_DIV_5;
	buf[icmd++] = TCK_DIVISOR;
	buf[icmd++] = divisor;
	buf[icmd++] = divisor >> 8;
	buf[icmd++] = SET_BITS_LOW;		  // opcode: set low bits (ADBUS[0-7])
	buf[icmd++] = pinInitialState;    // argument: inital pin states
	buf[icmd++] = pinDirection;
	if ((rc = ftdi_write_data(_ftdic, buf, icmd)) != icmd)
	{
		setInError(ftdi_get_error_string(_ftdic));
		return rc;
	}

	_isDeviceReady = true;
	return rc;
}

int ProviderFtdi::close()
{
	Debug(_log, "Closing FTDI device");
	if (_ftdic != nullptr) {
        QEventLoop loop;
//      Delay to give time to push color black from writeBlack() into the led,
//      otherwise frame transmission will be terminated half way through
        QTimer::singleShot(30, &loop, &QEventLoop::quit);
        loop.exec();

		ftdi_set_bitmode(_ftdic, 0x00, BITMODE_RESET);
		ftdi_usb_close(_ftdic);
		ftdi_free(_ftdic);
		_ftdic = nullptr;
	}
	return LedDevice::close();
}

void ProviderFtdi::setInError(const QString &errorMsg)
{
	close();

	LedDevice::setInError(errorMsg);
}

int ProviderFtdi::writeBytes(const qint64 size, const uint8_t *data)
{
	uint8_t buf[10] = {0};
	unsigned int icmd = 0;
	int rc = 0;

	int count_arg = size - 1;
	buf[icmd++] = SET_BITS_LOW;
	buf[icmd++] = pinInitialState & ~Pin::CS;
	buf[icmd++] = pinDirection;
	buf[icmd++] = MPSSE_DO_WRITE | MPSSE_WRITE_NEG;
	buf[icmd++] = count_arg;
	buf[icmd++] = count_arg >> 8;

	if ((rc = ftdi_write_data(_ftdic, buf, icmd)) != icmd)
	{
		setInError(ftdi_get_error_string(_ftdic));
		return rc;
	}
	if ((rc = ftdi_write_data(_ftdic, data, size)) != size)
	{
		setInError(ftdi_get_error_string(_ftdic));
		return rc;
	}
	icmd = 0;
	buf[icmd++] = SET_BITS_LOW;
	buf[icmd++] = pinInitialState | Pin::CS;
	buf[icmd++] = pinDirection;
	if ((rc = ftdi_write_data(_ftdic, buf, icmd)) != icmd)
	{
		setInError(ftdi_get_error_string(_ftdic));
		return rc;
	}
	return rc;
}

QJsonObject ProviderFtdi::discover(const QJsonObject & /*params*/)
{
	QJsonObject devicesDiscovered;
	QJsonArray deviceList;
	struct ftdi_device_list *devlist;
	struct ftdi_context *ftdic;

	QJsonObject autoDevice = QJsonObject{{"value", AUTO_SETTING}, {"name", "Auto"}};
	deviceList.push_back(autoDevice);

	ftdic = ftdi_new();

	if (ftdi_usb_find_all(ftdic, &devlist, ANY_FTDI_VENDOR, ANY_FTDI_PRODUCT) > 0)
	{
        QMap<QString, uint8_t> deviceIndexes;
		struct ftdi_device_list *curdev = devlist;
		while (curdev)
		{
			char manufacturer[128] = {0}, serial_string[128] = {0};
			ftdi_usb_get_strings(ftdic, curdev->dev, manufacturer, 128, NULL, 0, serial_string, 128);

            libusb_device_descriptor desc;
            libusb_get_device_descriptor(curdev->dev, &desc);

            QString vendorAndProduct = QString("0x%1:0x%2")
                    .arg(desc.idVendor, 4, 16, QChar{'0'})
                    .arg(desc.idProduct, 4, 16, QChar{'0'});
            
			QString serialNumber {serial_string};
			QString ftdiOpenString;
			if(!serialNumber.isEmpty())
			{
				ftdiOpenString = QString("s:%1:%2").arg(vendorAndProduct).arg(serialNumber);
			}
			else
			{
				uint8_t deviceIndex = deviceIndexes.value(vendorAndProduct, 0);
				ftdiOpenString = QString("i:%1:%2").arg(vendorAndProduct).arg(deviceIndex);
				deviceIndexes.insert(vendorAndProduct, deviceIndex + 1);
			}

            QString displayLabel = QString("%1 (%2)")
                    .arg(ftdiOpenString)
                    .arg(manufacturer);

			deviceList.push_back(QJsonObject{
				{"value", ftdiOpenString},
				{"name", displayLabel}
            });

			curdev = curdev->next;            
		}
	}

	ftdi_list_free(&devlist);
	ftdi_free(ftdic);

	devicesDiscovered.insert("ledDeviceType", _activeDeviceType);
	devicesDiscovered.insert("devices", deviceList);

	Debug(_log, "FTDI devices discovered: [%s]", QString(QJsonDocument(devicesDiscovered).toJson(QJsonDocument::Compact)).toUtf8().constData());

	return devicesDiscovered;
}