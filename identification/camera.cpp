/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the examples of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:BSD$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** BSD License Usage
** Alternatively, you may use this file under the terms of the BSD license
** as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of The Qt Company Ltd nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "camera.h"
#include "ui_camera.h"
#include "videosettings.h"
#include "imagesettings.h"

#include <QMediaService>
#include <QMediaRecorder>
#include <QCameraViewfinder>
#include <QCameraInfo>
#include <QMediaMetaData>

#include <QMessageBox>
#include <QPalette>

#include <QtWidgets>
#include <QDebug>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QFont>
#include <QColor>
#include <QBrush>
#include <QPen>
#include <QApplication>

Q_DECLARE_METATYPE(QCameraInfo)

Camera::Camera() : ui(new Ui::Camera)
{
    ui->setupUi(this);

    //Camera devices:

    QActionGroup *videoDevicesGroup = new QActionGroup(this);
    videoDevicesGroup->setExclusive(true);
    const QList<QCameraInfo> availableCameras = QCameraInfo::availableCameras();
    for (const QCameraInfo &cameraInfo : availableCameras) {
        QAction *videoDeviceAction = new QAction(cameraInfo.description(), videoDevicesGroup);
        videoDeviceAction->setCheckable(true);
        videoDeviceAction->setData(QVariant::fromValue(cameraInfo));
        if (cameraInfo == QCameraInfo::defaultCamera())
            videoDeviceAction->setChecked(true);

        ui->menuDevices->addAction(videoDeviceAction);
    }

    connect(videoDevicesGroup, &QActionGroup::triggered, this, &Camera::updateCameraDevice);
    connect(ui->captureWidget, &QTabWidget::currentChanged, this, &Camera::updateCaptureMode);

    // 初始化视频画布
    this->m_videocliper = new VideoCliper(this);
    this->m_scene = new QGraphicsScene();
    ui->graphicsView->setScene(this->m_scene);
    this->m_canvas = new QGraphicsPixmapItem();
    this->m_scene->addItem(this->m_canvas);
    this->m_videocliper->setCanvas(this->m_canvas);

    // 多线程
    this->m_thread.start();

    // 初始化人脸识别工具
    this->m_facerecognizer = new faceRecognizer();
    this->m_facerecognizer->setFaceDatabase("./database/namelist.csv");

    this->m_facerecognizer->moveToThread(&this->m_thread);

    // 连接信号处理-连接画布和人脸识别组件
    connect(this->m_videocliper, SIGNAL(frameAvailable(QImage)),
            this->m_facerecognizer, SLOT(faceRecognition(QImage)));

    // 初始化人脸标记组件
    this->m_numMarkUser = 10;   // 最大同时标注10名用户
    this->initGraphicsItems();  // 初始化

    setCamera(QCameraInfo::defaultCamera());
}

Camera::~Camera()
{
//    this->m_facerecognizer->moveToThread(QApplication::instance()->thread());
    this->m_thread.quit();
    this->m_thread.wait();
//    this->m_thread.exit(0);
}

void Camera::setCamera(const QCameraInfo &cameraInfo)
{
    m_camera.reset(new QCamera(cameraInfo));

    connect(m_camera.data(), &QCamera::stateChanged, this, &Camera::updateCameraState);
    connect(m_camera.data(), QOverload<QCamera::Error>::of(&QCamera::error), this, &Camera::displayCameraError);

    m_mediaRecorder.reset(new QMediaRecorder(m_camera.data()));
    connect(m_mediaRecorder.data(), &QMediaRecorder::stateChanged, this, &Camera::updateRecorderState);

    m_imageCapture.reset(new QCameraImageCapture(m_camera.data()));

    connect(m_mediaRecorder.data(), &QMediaRecorder::durationChanged, this, &Camera::updateRecordTime);
    connect(m_mediaRecorder.data(), QOverload<QMediaRecorder::Error>::of(&QMediaRecorder::error),
            this, &Camera::displayRecorderError);

    m_mediaRecorder->setMetaData(QMediaMetaData::Title, QVariant(QLatin1String("Test Title")));

    connect(ui->exposureCompensation, &QAbstractSlider::valueChanged, this, &Camera::setExposureCompensation);

    // 将画面显示在自定义板块上
//    m_camera->setViewfinder(ui->viewfinder);
    m_camera->setViewfinder(this->m_videocliper);   // Set the viewfinder

    updateCameraState(m_camera->state());
    updateLockStatus(m_camera->lockStatus(), QCamera::UserRequest);
    updateRecorderState(m_mediaRecorder->state());

    connect(m_imageCapture.data(), &QCameraImageCapture::readyForCaptureChanged, this, &Camera::readyForCapture);
    connect(m_imageCapture.data(), &QCameraImageCapture::imageCaptured, this, &Camera::processCapturedImage);
    connect(m_imageCapture.data(), &QCameraImageCapture::imageSaved, this, &Camera::imageSaved);
    connect(m_imageCapture.data(), QOverload<int, QCameraImageCapture::Error, const QString &>::of(&QCameraImageCapture::error),
            this, &Camera::displayCaptureError);

    connect(m_camera.data(), QOverload<QCamera::LockStatus, QCamera::LockChangeReason>::of(&QCamera::lockStatusChanged),
            this, &Camera::updateLockStatus);

    ui->captureWidget->setTabEnabled(0, (m_camera->isCaptureModeSupported(QCamera::CaptureStillImage)));
    ui->captureWidget->setTabEnabled(1, (m_camera->isCaptureModeSupported(QCamera::CaptureVideo)));

    updateCaptureMode();
    m_camera->start();
}

void Camera::keyPressEvent(QKeyEvent * event)
{
    if (event->isAutoRepeat())
        return;

    switch (event->key()) {
    case Qt::Key_CameraFocus:
        displayViewfinder();
        m_camera->searchAndLock();
        event->accept();
        break;
    case Qt::Key_Camera:
        if (m_camera->captureMode() == QCamera::CaptureStillImage) {
            takeImage();
        } else {
            if (m_mediaRecorder->state() == QMediaRecorder::RecordingState)
                stop();
            else
                record();
        }
        event->accept();
        break;
    default:
        QMainWindow::keyPressEvent(event);
    }
}

void Camera::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat())
        return;

    switch (event->key()) {
    case Qt::Key_CameraFocus:
        m_camera->unlock();
        break;
    default:
        QMainWindow::keyReleaseEvent(event);
    }
}

void Camera::updateRecordTime()
{
    QString str = QString("Recorded %1 sec").arg(m_mediaRecorder->duration()/1000);
    ui->statusbar->showMessage(str);
}

void Camera::processCapturedImage(int requestId, const QImage& img)
{
    Q_UNUSED(requestId);
//    QImage scaledImage = img.scaled(ui->viewfinder->size(),
//                                    Qt::KeepAspectRatio,
//                                    Qt::SmoothTransformation);
    QImage scaledImage = QImage(img);

    ui->lastImagePreviewLabel->setPixmap(QPixmap::fromImage(scaledImage));

    // Display captured image for 4 seconds.
    displayCapturedImage();
    QTimer::singleShot(4000, this, &Camera::displayViewfinder);
}

void Camera::configureCaptureSettings()
{
    switch (m_camera->captureMode()) {
    case QCamera::CaptureStillImage:
        configureImageSettings();
        break;
    case QCamera::CaptureVideo:
        configureVideoSettings();
        break;
    default:
        break;
    }
}

void Camera::configureVideoSettings()
{
    VideoSettings settingsDialog(m_mediaRecorder.data());
    settingsDialog.setWindowFlags(settingsDialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    settingsDialog.setAudioSettings(m_audioSettings);
    settingsDialog.setVideoSettings(m_videoSettings);
    settingsDialog.setFormat(m_videoContainerFormat);

    if (settingsDialog.exec()) {
        m_audioSettings = settingsDialog.audioSettings();
        m_videoSettings = settingsDialog.videoSettings();
        m_videoContainerFormat = settingsDialog.format();

        m_mediaRecorder->setEncodingSettings(
                    m_audioSettings,
                    m_videoSettings,
                    m_videoContainerFormat);
    }
}

void Camera::configureImageSettings()
{
    ImageSettings settingsDialog(m_imageCapture.data());
    settingsDialog.setWindowFlags(settingsDialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    settingsDialog.setImageSettings(m_imageSettings);

    // 设置检测频率和阈值
    settingsDialog.setDetectInterval(this->m_videocliper->frameInterval());
    settingsDialog.setThreshold(this->m_facerecognizer->threshold());

    if (settingsDialog.exec()) {
        m_imageSettings = settingsDialog.imageSettings();
        m_imageCapture->setEncodingSettings(m_imageSettings);

        // 设置检测频率和人脸检测阈值
        this->m_videocliper->setFrameInterval(settingsDialog.detectInterval());
        this->m_facerecognizer->setThreshold(settingsDialog.threshold());
    }
}

void Camera::record()
{
    m_mediaRecorder->record();
    updateRecordTime();
}

void Camera::pause()
{
    m_mediaRecorder->pause();
}

void Camera::stop()
{
    m_mediaRecorder->stop();
}

void Camera::setMuted(bool muted)
{
    m_mediaRecorder->setMuted(muted);
}

void Camera::toggleLock()
{
    switch (m_camera->lockStatus()) {
    case QCamera::Searching:
    case QCamera::Locked:
        m_camera->unlock();
        break;
    case QCamera::Unlocked:
        m_camera->searchAndLock();
    }
}

void Camera::updateLockStatus(QCamera::LockStatus status, QCamera::LockChangeReason reason)
{
    QColor indicationColor = Qt::black;

    switch (status) {
    case QCamera::Searching:
        indicationColor = Qt::yellow;
        ui->statusbar->showMessage(tr("Focusing..."));
        ui->lockButton->setText(tr("Focusing..."));
        break;
    case QCamera::Locked:
        indicationColor = Qt::darkGreen;
        ui->lockButton->setText(tr("Unlock"));
        ui->statusbar->showMessage(tr("Focused"), 2000);
        break;
    case QCamera::Unlocked:
        indicationColor = reason == QCamera::LockFailed ? Qt::red : Qt::black;
        ui->lockButton->setText(tr("Focus"));
        if (reason == QCamera::LockFailed)
            ui->statusbar->showMessage(tr("Focus Failed"), 2000);
    }

    QPalette palette = ui->lockButton->palette();
    palette.setColor(QPalette::ButtonText, indicationColor);
    ui->lockButton->setPalette(palette);
}

void Camera::takeImage()
{
    m_isCapturingImage = true;
    m_imageCapture->capture();
}

void Camera::displayCaptureError(int id, const QCameraImageCapture::Error error, const QString &errorString)
{
    Q_UNUSED(id);
    Q_UNUSED(error);
    QMessageBox::warning(this, tr("Image Capture Error"), errorString);
    m_isCapturingImage = false;
}

void Camera::startCamera()
{
    m_camera->start();
}

void Camera::stopCamera()
{
    m_camera->stop();
}

void Camera::updateCaptureMode()
{
    int tabIndex = ui->captureWidget->currentIndex();
    QCamera::CaptureModes captureMode = tabIndex == 0 ? QCamera::CaptureStillImage : QCamera::CaptureVideo;

    if (m_camera->isCaptureModeSupported(captureMode))
        m_camera->setCaptureMode(captureMode);
}

void Camera::updateCameraState(QCamera::State state)
{
    switch (state) {
    case QCamera::ActiveState:
        ui->actionStartCamera->setEnabled(false);
        ui->actionStopCamera->setEnabled(true);
        ui->captureWidget->setEnabled(true);
        ui->actionSettings->setEnabled(true);
        break;
    case QCamera::UnloadedState:
    case QCamera::LoadedState:
        ui->actionStartCamera->setEnabled(true);
        ui->actionStopCamera->setEnabled(false);
        ui->captureWidget->setEnabled(false);
        ui->actionSettings->setEnabled(false);
    }
}

void Camera::updateRecorderState(QMediaRecorder::State state)
{
    switch (state) {
    case QMediaRecorder::StoppedState:
        ui->recordButton->setEnabled(true);
        ui->pauseButton->setEnabled(true);
        ui->stopButton->setEnabled(false);
        break;
    case QMediaRecorder::PausedState:
        ui->recordButton->setEnabled(true);
        ui->pauseButton->setEnabled(false);
        ui->stopButton->setEnabled(true);
        break;
    case QMediaRecorder::RecordingState:
        ui->recordButton->setEnabled(false);
        ui->pauseButton->setEnabled(true);
        ui->stopButton->setEnabled(true);
        break;
    }
}

void Camera::setExposureCompensation(int index)
{
    m_camera->exposure()->setExposureCompensation(index*0.5);
}

void Camera::displayRecorderError()
{
    QMessageBox::warning(this, tr("Capture Error"), m_mediaRecorder->errorString());
}

void Camera::displayCameraError()
{
    QMessageBox::warning(this, tr("Camera Error"), m_camera->errorString());
}

void Camera::updateCameraDevice(QAction *action)
{
    setCamera(qvariant_cast<QCameraInfo>(action->data()));
}

void Camera::displayViewfinder()
{
    ui->stackedWidget->setCurrentIndex(0);
}

void Camera::displayCapturedImage()
{
    ui->stackedWidget->setCurrentIndex(1);
}

void Camera::readyForCapture(bool ready)
{
    ui->takeImageButton->setEnabled(ready);
}

void Camera::imageSaved(int id, const QString &fileName)
{
    Q_UNUSED(id);
    ui->statusbar->showMessage(tr("Captured \"%1\"").arg(QDir::toNativeSeparators(fileName)));

    m_isCapturingImage = false;
    if (m_applicationExiting)
        close();
}

///
/// \brief Camera::drawRecongnitionResult
/// 在画布上标记识别出的用户的信息
/// 用普通颜色标记有备案的用户
/// 用 warning 颜色标识未知用户
/// \param rects
/// \param userinfos
///
void Camera::drawRecongnitionResult(QVector<QRectF> rects, QVector<UserInfo> userinfos)
{
//    qDebug() << "draw recongnition result!";

    // 为了限制，做了一个最大的数量
    int size = this->m_numMarkUser>rects.size()?rects.size():this->m_numMarkUser;

    // 设置不同的标记颜色
    QColor rect_safe(0,255,0);
    QColor warning(255,0,0);

    for(int i = 0; i < size; i++)
    {
        // 处理标记

        // 画框
        QGraphicsRectItem *graphicsRect = this->m_graphicsRects[i];
        graphicsRect->setRect(rects[i]);


        // 标记文字
        QGraphicsSimpleTextItem *simpleText = this->m_graphicsTexts[i];
        simpleText->setText(userinfos[i].toSimpleString());
        simpleText->setPos(rects[i].left(),rects[i].bottom());


        // 设置颜色
        if(userinfos[i].isUnknown())
        {
            // warning
            QPen rectPen = graphicsRect->pen();
            rectPen.setColor(warning);
            graphicsRect->setPen(rectPen);

        }
        else
        {
            // safe
            QPen rectPen = graphicsRect->pen();
            rectPen.setColor(rect_safe);
            graphicsRect->setPen(rectPen);

        }

        graphicsRect->setVisible(true);      // 设置可见
        simpleText->setVisible(true);

    }

    for(int i = size; i < this->m_numMarkUser; i++)
    {
        // 将多余的组件隐藏
        QGraphicsRectItem *graphicsRect = this->m_graphicsRects[i];
        graphicsRect->setVisible(false);

        QGraphicsSimpleTextItem *simpleText = this->m_graphicsTexts[i];
        simpleText->setVisible(false);
    }

}

void Camera::closeEvent(QCloseEvent *event)
{
    if (m_isCapturingImage) {
        setEnabled(false);
        m_applicationExiting = true;
        event->ignore();
    } else {
        event->accept();
    }
}

///
/// \brief Camera::initGraphicsItems
/// 初始化人脸标注用的方框和文字
///
void Camera::initGraphicsItems()
{
    qDebug() << "Init graphics Items";

    for(int i = 0; i < this->m_numMarkUser; i++)
    {
        QGraphicsRectItem *graphicsRect = new QGraphicsRectItem();
        this->m_scene->addItem(graphicsRect);
        graphicsRect->setZValue(500);
        graphicsRect->setVisible(true);
        QPen rectPen = graphicsRect->pen();

        // 设置样式
        rectPen.setWidth(3);
        graphicsRect->setPen(rectPen);

        this->m_graphicsRects.push_back(graphicsRect);

        QGraphicsSimpleTextItem *simpleText = new QGraphicsSimpleTextItem();
        this->m_scene->addItem(simpleText);
        simpleText->setZValue(600);
        simpleText->setVisible(true);

        QFont font = simpleText->font();
        font.setPointSize(20);
        simpleText->setFont(font);

        this->m_graphicsTexts.push_back(simpleText);

    }

    // 建立连接
    int metatype_id_rect = qRegisterMetaType<QVector<QRectF>>("QVector<QRectF>");
    int metatype_id_userinfo = qRegisterMetaType<QVector<UserInfo>>("QVector<UserInfo>");

    connect(this->m_facerecognizer, SIGNAL(recongnitionResult(QVector<QRectF>, QVector<UserInfo>)),
            this, SLOT(drawRecongnitionResult(QVector<QRectF>, QVector<UserInfo>)));
}
