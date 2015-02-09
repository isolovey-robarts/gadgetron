#ifndef ISMRMRDMESSAGES_H
#define ISMRMRDMESSAGES_H

#include "ace/SOCK_Stream.h"
#include "ace/Task.h"

#include <complex>
#include <stdexcept>
#include <sstream>

#include "GadgetMRIHeaders.h"
#include "GadgetContainerMessage.h"
#include "GadgetMessageReaderWriter.h"
#include "GadgetronExport.h"
#include "gadgetron_mricore_export.h"
#include "log.h"

#include "ismrmrd/ismrmrd.h"
#include "ismrmrd/stream.h"

/**
These Reader/Writer classes are designed to support diverse platforms.
Gadgetron is a cross-platform messaging system and therefore calls of
the form

send(&object, sizeof(object));

are not acceptable because

1) Sending/Receiving platforms may have different endianness
2) Memory layout of object is compiler/platform dependent

Although other parts of Gadgetron do not take these critical issues
into consideration, the design of these classes should permit appropriate
cross-platform communication.

ISMRMRD has been updated to provide endian compatibility and memory layout
agnostic streaming of its objects.
*/

namespace Gadgetron
{

/**
Default implementation of GadgetMessageWriter for ISMRMRD images
*/

template <typename T> class EXPORTGADGETSMRICORE IsmrmrdImageMessageWriter : public GadgetMessageWriter
{
public:
    virtual int write(ACE_SOCK_Stream* sock, ACE_Message_Block* mb)
    {
        GadgetContainerMessage< ISMRMRD::Image<T> >* imagemb = AsContainerMessage< ISMRMRD::Image<T> > (mb);

        if (!imagemb) {
            GERROR("IsmrmrdImageMessageWriter invalid message block\n");
            return -1;
        }

        ssize_t send_cnt = 0;
        GadgetMessageIdentifier id;

        switch (sizeof(T)) {
        case 2: //Unsigned short
            id.id = GADGET_MESSAGE_ISMRMRD_IMAGEWITHATTRIB_REAL_USHORT;
            break;
        case 4: //Float
            id.id = GADGET_MESSAGE_ISMRMRD_IMAGEWITHATTRIB_REAL_FLOAT;
            break;
        case 8: //Complex float
            id.id = GADGET_MESSAGE_ISMRMRD_IMAGEWITHATTRIB_CPLX_FLOAT;
            break;
        default:
            GERROR("IsmrmrdImageMessageWriter detected invalid type size\n");
            return -1;
        }

        // Note that this is NOT a cross-platform compatible call
        // Unfortunately it is the way Gadgetron has been implemented and therefore
        // is used until such a time as Gadgetron correctly implements cross-platform
        // support throughout
        if ((send_cnt = sock->send_n (&id, sizeof(GadgetMessageIdentifier))) <= 0) {
            GERROR("IsmrmrdImageMessageWriter unable to send message identifier\n");
            return -1;
        }

        // Get a pointer to the Image
        ISMRMRD::Image<T> *pImage = imagemb->getObjectPtr();

        // Get reference to the ImageHeader
        const ISMRMRD::ImageHeader &header = pImage->getHead();

        // Send the ImageHeader into a stringstream
        // This makes a copy but is simpler than writing a streambuf
        // on top of an ACE_SOCK_Stream which would be preferable
        std::ostringstream headerStream;

        try {
            headerStream << header;
        }
        catch (std::exception &e) {
            GERROR("IsmrmrdImageMessageWriter unable to stream ISMRMRD::ImageHeader: %s\n", e.what());
            return -1;
        }

        // Create a contiguous string representing the header information from the stringstream
        std::string headerString = headerStream.str();

        // To simplify cross-platform and cross-version support we need to record the size
        // of the header in stream format, as different sending/receiving units may be using
        // different ISMRMRD version and therefore have different sizes
        uint16_t headerSize = static_cast<uint16_t>(headerString.length());

        // Convert the number to stream format
        ISMRMRD::convertToStream(&headerSize, 1);

        // Send the ImageHeader size
        if ((send_cnt = sock->send_n (reinterpret_cast<char *>(&headerSize), sizeof(headerSize))) <= 0) {
            GERROR("IsmrmrdImageMessageWriter unable to send ISMRMRD::ImageHeader size\n");
            return -1;
        }

        // Send the string version of the header
        if ((send_cnt = sock->send_n (headerString.c_str(), headerString.length())) <= 0) {
            GERROR("IsmrmrdImageMessageWriter unable to send ISMRMRD::ImageHeader\n");
            return -1;
        }

        // Now send the attribute_string information
        // This is always narrow bytes so endianness is not an issue
        std::string attribute_string;
        pImage->getAttributeString(attribute_string);

        // Send the attribute string, if not null
        if (attribute_string.length() && (send_cnt = sock->send_n (attribute_string.c_str(), attribute_string.length())) <= 0) {
            GERROR("IsmrmrdImageMessageWriter unable to send ISMRMRD::Image<T> attribute string\n");
            return -1;
        }

        // Finally send the data
        T * pData = pImage->getDataPtr();
        size_t numElements = pImage->getNumberOfDataElements();

        // Convert the data to stream format in place
        ISMRMRD::convertToStream(pData, numElements);

        if ((send_cnt = sock->send_n (reinterpret_cast<char*>(pData), pImage->getDataSize())) <= 0) {
            GERROR("IsmrmrdImageMessageWriter unable to send ISMRMRD::Image<T> data\n");
            return -1;
        }

        // Undo the conversion to stream format
        ISMRMRD::convertFromStream(pData, numElements);

        return 0;
    }

};

template <typename T> class IsmrmrdImageMessageReader : public GadgetMessageReader
{
public:
    IsmrmrdImageMessageReader() {}

protected:
    virtual ACE_Message_Block* read(ACE_SOCK_Stream* stream) {
        GadgetContainerMessage< ISMRMRD::Image<T> > *m1 =
            new GadgetContainerMessage< ISMRMRD::Image<T> >();

        ssize_t recv_count = 0;

        // Get pointer to the image
        ISMRMRD::Image<T> *pImage = m1->getObjectPtr();

        // Get reference to the header
        ISMRMRD::ImageHeader &header = pImage->getHead();

        // Read the Header from the stream first
        uint16_t headerSize;

        // Get the size of the header
        if ((recv_count = stream->recv_n(&headerSize, sizeof(headerSize))) <= 0) {
            GERROR("IsmrmrdImageMessageReader unable to read ISMRMRD::ImageHeader size\n");
            m1->release();
            return NULL;
        }

        // Convert from the stream format to native
        ISMRMRD::convertFromStream(&(headerSize), 1);

        // Read in the ImageHeader data into a char buffer
        char *headerBuffer = new char [headerSize];

        // Read the header into the buffer
        if ((recv_count = stream->recv_n(headerBuffer, headerSize)) <= 0) {
            GERROR("IsmrmrdImageMessageReader unable to read ISMRMRD::ImageHeader\n");
            m1->release();
            return NULL;
        }

        try {
            // Wrap the char array in a streambuf
            ISMRMRD::char_array_buffer buf(headerBuffer, headerSize);

            // Create an input stream reading from the buffer
            std::istream headerStream(&buf);

            // Get the header from the stream
            headerStream >> header;

            // Set the ImageHeader
            pImage->setHead(header);
        }
        catch (std::exception &e) {
            GERROR("IsmrmrdImageMessageReader unable to set ISMRMRD::ImageHeader: %s\n", e.what());
            delete [] headerBuffer;
            m1->release();
            return NULL;
        }

        // Delete the temporary memory
        delete [] headerBuffer;

        // Now get the size of the attribute string
        size_t attributeSize = pImage->getAttributeStringLength();

        if (attributeSize) {
            // Allocate space including a null terminating character
            char *attributeBuffer = new char [attributeSize+1];

            // Read the attribute string from the stream
            if ((recv_count = stream->recv_n(attributeBuffer, attributeSize)) <= 0) {
                GERROR("IsmrmrdImageMessageReader unable to read ISMRMRD::Image<T> attribute string\n");
                m1->release();
                return NULL;
            }
            // Force null termination
            attributeBuffer[attributeSize] = '\0';

            // Set the attribute string
            pImage->setAttributeString(attributeBuffer);
            delete [] attributeBuffer;
        }

        // Read the data in directly, no assignment needed
        T *pData = pImage->getDataPtr();
        if ((recv_count = stream->recv_n(reinterpret_cast<char *>(pData), pImage->getDataSize())) <=0) {
            GERROR("IsmrmrdImageMessageReader unable to read ISMRMRD::Image data\n");
            m1->release();
            return NULL;
        }

        ISMRMRD::convertFromStream(pData, pImage->getNumberOfDataElements());
        return m1;
    }
};

// Default Writer classes
class EXPORTGADGETSMRICORE IsmrmrdImageMessageWriterUSHORT :
public IsmrmrdImageMessageWriter<uint16_t>
{
public:
    GADGETRON_WRITER_DECLARE(IsmrmrdImageMessageWriterUSHORT)
};

class EXPORTGADGETSMRICORE IsmrmrdImageMessageWriterFLOAT :
public IsmrmrdImageMessageWriter<float>
{
public:
    GADGETRON_WRITER_DECLARE(IsmrmrdImageMessageWriterFLOAT)
};

class EXPORTGADGETSMRICORE IsmrmrdImageMessageWriterCPLX :
public IsmrmrdImageMessageWriter< complex_float_t >
{
public:
    GADGETRON_WRITER_DECLARE(IsmrmrdImageMessageWriterCPLX)
};

// Default Reader classes
class EXPORTGADGETSMRICORE IsmrmrdImageMessageReaderUSHORT :
public IsmrmrdImageMessageReader<uint16_t>
{
public:
    GADGETRON_READER_DECLARE(IsmrmrdImageMessageReaderUSHORT)
};

class EXPORTGADGETSMRICORE IsmrmrdImageMessageReaderFLOAT :
public IsmrmrdImageMessageReader<float>
{
public:
    GADGETRON_READER_DECLARE(IsmrmrdImageMessageReaderFLOAT)
};

class EXPORTGADGETSMRICORE IsmrmrdImageMessageReaderCPLX :
public IsmrmrdImageMessageReader< complex_float_t >
{
public:
    GADGETRON_READER_DECLARE(IsmrmrdImageMessageReaderCPLX)
};


/**
Default implementation of GadgetMessageWriter for ISMRMRD acquisitions
*/
class EXPORTGADGETSMRICORE IsmrmrdAcquisitionMessageWriter : public GadgetMessageWriter
{

public:
    virtual int write(ACE_SOCK_Stream* sock, ACE_Message_Block* mb)
    {
        GadgetContainerMessage< ISMRMRD::Acquisition >* acqmb = AsContainerMessage<ISMRMRD::Acquisition>(mb);

        if (!acqmb) {
            GERROR("IsmrmrdAcquisitionMessageWriter invalid acquisition message objects\n");
            return -1;
        }

        ssize_t send_cnt = 0;

        GadgetMessageIdentifier id;
        id.id = GADGET_MESSAGE_ISMRMRD_ACQUISITION;

        // Note that this is NOT a cross-platform compatible call
        // Unfortunately it is the way Gadgetron has been implemented and therefore
        // is used until such a time as Gadgetron correctly implements cross-platform
        // support throughout
        if ((send_cnt = sock->send_n (&id, sizeof(GadgetMessageIdentifier))) <= 0) {
            GERROR("IsmrmrdAcquisitionMessageWriter unable to send message identifier\n");
            return -1;
        }

        ISMRMRD::Acquisition *acq = acqmb->getObjectPtr();
        const ISMRMRD::AcquisitionHeader &header = acq->getHead();

        // Send the ImageHeader into a stringstream
        // This makes a copy but is simpler than writing a streambuf
        // on top of an ACE_SOCK_Stream
        std::ostringstream headerStream;

        try {
            headerStream << header;
        } catch (std::exception &e) {
            GERROR("IsmrmrdAcquisitionMessageWriter unable to stream ISMRMRD::AcquisitionHeader: %e\n", e.what());
            return -1;
        }
        // Create a contiguous string representing the header information from the stringstream
        std::string headerString = headerStream.str();

        // To simplify cross-platform and cross-version support we need to record the size
        // of the header in stream format, as different sending/receiving units may be using
        // different ISMRMRD versions and therefore have different sizes
        uint16_t headerSize = static_cast<uint16_t>(headerString.length());

        // Convert the number to network format
        ISMRMRD::convertFromStream(&(headerSize), 1);

        // Send the ImageHeader size
        if ((send_cnt = sock->send_n (reinterpret_cast<char *>(&headerSize), sizeof(headerSize))) <= 0) {
            GERROR("IsmrmrdAcquisitionMessageWriter unable to send ISMRMRD::AcquisitionHeader size\n");
            return -1;
        }

        // Send the string version of the header
        if ((send_cnt = sock->send_n (headerString.c_str(), headerString.length())) <= 0) {
            GERROR("IsmrmrdAcquisitionMessageWriter unable to send ISMRMRD::AcquisitionHeader\n");
            return -1;
        }

        // Only output data if it exist
        if (acq->getNumberOfDataElements()) {
            // Conversion function may require that pData be changed
            complex_float_t *pData = const_cast<complex_float_t *>(acq->getDataPtr());

            // Convert to stream format, in-place
            ISMRMRD::convertToStream(pData, acq->getNumberOfDataElements());

            if ((send_cnt = sock->send_n (reinterpret_cast<const void *>(pData), acq->getDataSize())) <= 0) {
                GERROR("IsmrmrdAcquisitionMessageWriter unable to send ISMRMRD::Acquisition data\n");
                return -1;
            }

            // Convert from stream format, in-place
            // This would not be required if the message gets deleted immediately
            ISMRMRD::convertFromStream(pData, acq->getNumberOfDataElements());
        }

        if (acq->getNumberOfTrajElements()) {
            float *pTraj = const_cast<float *>(acq->getTrajPtr());
            ISMRMRD::convertToStream(pTraj, acq->getNumberOfTrajElements());
            if ((send_cnt = sock->send_n (reinterpret_cast<void *>(pTraj), acq->getTrajSize())) <= 0) {
                GERROR("IsmrmrdAcquisitionMessageWriter unable to send ISMRMRD::Acquisition trajectory\n");
                return -1;
            }
            ISMRMRD::convertFromStream(pTraj, acq->getNumberOfTrajElements());
        }

        return 0;
    }
};

/**
Default implementation of GadgetMessageReader for IsmrmrdAcquisition messages
*/
class EXPORTGADGETSMRICORE IsmrmrdAcquisitionMessageReader : public GadgetMessageReader
{

public:
    GADGETRON_READER_DECLARE(IsmrmrdAcquisitionMessageReader);

    virtual ACE_Message_Block* read(ACE_SOCK_Stream* stream)
    {

        GadgetContainerMessage<ISMRMRD::Acquisition>* m1 = new GadgetContainerMessage<ISMRMRD::Acquisition>();

        ssize_t recv_count = 0;

        ISMRMRD::Acquisition *acq = m1->getObjectPtr();
        ISMRMRD::AcquisitionHeader &header = const_cast<ISMRMRD::AcquisitionHeader &> (acq->getHead());

        // Read the Header from the stream first
        uint16_t headerSize;

        if ((recv_count = stream->recv_n(&headerSize, sizeof(headerSize))) <= 0) {
            GERROR("IsmrmrdAcquisitionMessageReader unable to read ISMRMRD::AcquisitionHeader\n");
            m1->release();
            return NULL;
        }

        // Convert stream to host endianness
        ISMRMRD::convertFromStream(&(headerSize), 1);

        // Allocate space for the buffer
        char *headerBuffer = new char [headerSize];

        // Read the data into a buffer
        if ((recv_count = stream->recv_n(headerBuffer, headerSize)) <= 0) {
            GERROR("IsmrmrdAcquisitionMessageReader unable to read ISMRMRD::AcquisitionHeader\n");
            m1->release();
            return NULL;
        }

        try {
            // Wrap the char array in a streambuf
            ISMRMRD::char_array_buffer buf(headerBuffer, headerSize);

            // Create an input stream reading from the buffer
            std::istream headerStream(&buf);

            // Get the header from the stream
            headerStream >> header;

            // Set the header
            acq->setHead(header);
        } catch (std::exception &e) {
            GERROR("IsmrmrdAcquisitionMessageReader unable to set ISMRMRD::AcquisitionHeader: %e\n", e.what());
            delete [] headerBuffer;
            m1->release();
            return NULL;
        }

        // Delete the temporary memory
        delete [] headerBuffer;

        if (acq->getDataSize()) {
            // Read the data in directly
            // acq has already allocated the required memory
            complex_float_t *pData = const_cast<complex_float_t *>(acq->getDataPtr());
            if ((recv_count = stream->recv_n(reinterpret_cast<char *>(pData), acq->getDataSize())) <=0) {
                GERROR("IsmrmrdAcquisitionMessageReader unable to read ISMRMRD::Acquisition data\n");
                m1->release();
                return NULL;
            }

            // Convert from stream to host endianness
            ISMRMRD::convertFromStream(pData, acq->getNumberOfDataElements());
        }

        if (acq->getTrajSize()) {
            // Read the trajectory in directly
            // acq has already allocated the required memory
            float *pTraj = const_cast<float *>(acq->getTrajPtr());
            if ((recv_count = stream->recv_n(reinterpret_cast<char *>(pTraj), acq->getTrajSize())) <=0) {
                GERROR("IsmrmrdAcquisitionMessageReader unable to read ISMRMRD::Acquisition trajectory\n");
                m1->release();
                return NULL;
            }

            // Convert from stream to host endianness
            ISMRMRD::convertFromStream(pTraj, acq->getNumberOfTrajElements());
        }

        return m1;
    }

};

}

#endif //GADGETIMAGEMESSAGEWRITER_H
