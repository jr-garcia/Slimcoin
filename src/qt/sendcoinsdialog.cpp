#include "sendcoinsdialog.h"
#include "burncoinsdialog.h"
#include "ui_sendcoinsdialog.h"
#include "walletmodel.h"
#include "bitcoinunits.h"
#include "addressbookpage.h"
#include "optionsmodel.h"
#include "sendcoinsentry.h"
#include "guiutil.h"
#include "askpassphrasedialog.h"

#include <QMessageBox>
#include <QLocale>
#include <QTextDocument>
#include <QScrollBar>

SendCoinsDialog::SendCoinsDialog(QWidget *parent) :
  QDialog(parent),
  ui(new Ui::SendCoinsDialog),
  model(0)
{
  ui->setupUi(this);

#ifdef Q_OS_MAC // Icons on push buttons are very uncommon on Mac
  ui->addButton->setIcon(QIcon());
  ui->clearButton->setIcon(QIcon());
  ui->sendButton->setIcon(QIcon());
#endif

  addEntry();

  connect(ui->addButton, SIGNAL(clicked()), this, SLOT(addEntry()));
  connect(ui->clearButton, SIGNAL(clicked()), this, SLOT(clear()));

  fNewRecipientAllowed = true;
}

void SendCoinsDialog::setModel(WalletModel *model)
{
  this->model = model;

  for(int i = 0; i < ui->entries->count(); ++i)
  {
    SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
    if(entry)
    {
      entry->setModel(model);
    }
  }
  if(model)
  {
    setBalance(model->getBalance(), model->getStake(), model->getUnconfirmedBalance(), model->getReserveBalance());
    connect(model, SIGNAL(balanceChanged(qint64, qint64, qint64, qint64, BurnCoinsBalances)), this, SLOT(setBalance(qint64, qint64, qint64, qint64)));
    // connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
  }
}

SendCoinsDialog::~SendCoinsDialog()
{
  delete ui;
}

void SendCoinsDialog::on_sendButton_clicked()
{
  QList<SendCoinsRecipient> recipients;
  bool valid = true;

  if(!model)
    return;

  for(int i = 0; i < ui->entries->count(); ++i)
  {
    SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
    if(entry)
    {
      if(entry->validate())
      {
        recipients.append(entry->getValue());
      }
      else
      {
        valid = false;
      }
    }
  }

  if(!valid || recipients.isEmpty())
  {
    return;
  }

  // Format confirmation message
  QStringList formatted;
  foreach(const SendCoinsRecipient &rcp, recipients)
  {
#if QT_VERSION < 0x050000
    formatted.append(tr("<b>%1</b> to %2 (%3)").arg(BitcoinUnits::formatWithUnit(BitcoinUnits::BTC, rcp.amount), Qt::escape(rcp.label), rcp.address));
#else
    formatted.append(tr("<b>%1</b> to %2 (%3)").arg(BitcoinUnits::formatWithUnit(BitcoinUnits::BTC, rcp.amount), rcp.label.toHtmlEscaped(), rcp.address));
#endif
  }

  fNewRecipientAllowed = false;

  QMessageBox::StandardButton retval = QMessageBox::question(this, tr("Confirm send coins"),
                                                             tr("Are you sure you want to send %1?").arg(formatted.join(tr(" and "))),
                                                             QMessageBox::Yes|QMessageBox::Cancel,
                                                             QMessageBox::Cancel);

  if(retval != QMessageBox::Yes)
  {
    fNewRecipientAllowed = true;
    return;
  }

  WalletModel::UnlockContext ctx(model->requestUnlock());
  if(!ctx.isValid())
  {
    // Unlock wallet was cancelled
    fNewRecipientAllowed = true;
    return;
  }

  //the false indicates this is supposed to not be a burn transaction
  //also pass an empty string as txtmsg
  QString txtmsg = "";
  WalletModel::SendCoinsReturn sendstatus = model->sendCoins(recipients, txtmsg, false);
  switch(sendstatus.status)
  {
  case WalletModel::InvalidAddress:
    QMessageBox::warning(this, tr("Send Coins"),
                         tr("The recepient address is not valid, please recheck."),
                         QMessageBox::Ok, QMessageBox::Ok);
    break;
  case WalletModel::InvalidAmount:
    QMessageBox::warning(this, tr("Send Coins"),
                         tr("The amount to pay must be at least one cent (0.01)."),
                         QMessageBox::Ok, QMessageBox::Ok);
    break;
  case WalletModel::AmountExceedsBalance:
    QMessageBox::warning(this, tr("Send Coins"),
                         tr("Amount exceeds your balance"),
                         QMessageBox::Ok, QMessageBox::Ok);
    break;
  case WalletModel::AmountWithFeeExceedsBalance:
    QMessageBox::warning(this, tr("Send Coins"),
                         tr("Total exceeds your balance when the %1 transaction fee is included").
                         arg(BitcoinUnits::formatWithUnit(BitcoinUnits::BTC, sendstatus.fee)),
                         QMessageBox::Ok, QMessageBox::Ok);
    break;
  case WalletModel::DuplicateAddress:
    QMessageBox::warning(this, tr("Send Coins"),
                         tr("Duplicate address found, can only send to each address once in one send operation"),
                         QMessageBox::Ok, QMessageBox::Ok);
    break;
  case WalletModel::TransactionCreationFailed:
    QMessageBox::warning(this, tr("Send Coins"),
                         tr("Error: Transaction creation failed  "),
                         QMessageBox::Ok, QMessageBox::Ok);
    break;
  case WalletModel::TransactionCommitFailed:
    QMessageBox::warning(this, tr("Send Coins"),
                         tr("Error: The transaction was rejected.  This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here."),
                         QMessageBox::Ok, QMessageBox::Ok);
    break;
  case WalletModel::BadBurningCoins:
    QMessageBox::warning(this, tr("Send Coins"),
                         tr("You are sending coins to a burn address without using the dedicated \"" BURN_COINS_DIALOG_NAME "\" tab. If you want to burn coins, use the dedicated tab instead of this \"" SEND_COINS_DIALOG_NAME "\" tab. \n\nSending coins aborted."),
                         QMessageBox::Ok, QMessageBox::Ok);
    break;
  case WalletModel::Aborted: // User aborted, nothing to do
    break;
  case WalletModel::OK:
    accept();
    break;
  }
  fNewRecipientAllowed = true;
}

void SendCoinsDialog::clear()
{
  // Remove entries until only one left
  while(ui->entries->count())
  {
    delete ui->entries->takeAt(0)->widget();
  }
  addEntry();

  updateRemoveEnabled();

  ui->sendButton->setDefault(true);
}

void SendCoinsDialog::reject()
{
  clear();
}

void SendCoinsDialog::accept()
{
  clear();
}

SendCoinsEntry *SendCoinsDialog::addEntry()
{
  SendCoinsEntry *entry = new SendCoinsEntry(this);
  entry->setModel(model);
  ui->entries->addWidget(entry);
  connect(entry, SIGNAL(removeEntry(SendCoinsEntry*)), this, SLOT(removeEntry(SendCoinsEntry*)));

  updateRemoveEnabled();

  // Focus the field, so that entry can start immediately
  entry->clear();
  entry->setFocus();
  ui->scrollAreaWidgetContents->resize(ui->scrollAreaWidgetContents->sizeHint());
  QCoreApplication::instance()->processEvents();
  QScrollBar* bar = ui->scrollArea->verticalScrollBar();
  if(bar)
    bar->setSliderPosition(bar->maximum());
  return entry;
}

void SendCoinsDialog::updateRemoveEnabled()
{
  // Remove buttons are enabled as soon as there is more than one send-entry
  bool enabled = (ui->entries->count() > 1);
  for(int i = 0; i < ui->entries->count(); ++i)
  {
    SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
    if(entry)
    {
      entry->setRemoveEnabled(enabled);
    }
  }
  setupTabChain(0);
}

void SendCoinsDialog::removeEntry(SendCoinsEntry* entry)
{
  delete entry;
  updateRemoveEnabled();
}

QWidget *SendCoinsDialog::setupTabChain(QWidget *prev)
{
  for(int i = 0; i < ui->entries->count(); ++i)
  {
    SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
    if(entry)
    {
      prev = entry->setupTabChain(prev);
    }
  }
  QWidget::setTabOrder(prev, ui->addButton);
  QWidget::setTabOrder(ui->addButton, ui->sendButton);
  return ui->sendButton;
}

void SendCoinsDialog::pasteEntry(const SendCoinsRecipient &rv)
{
  if(!fNewRecipientAllowed)
    return;

  SendCoinsEntry *entry = 0;
  // Replace the first entry if it is still unused
  if(ui->entries->count() == 1)
  {
    SendCoinsEntry *first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
    if(first->isClear())
    {
      entry = first;
    }
  }
  if(!entry)
  {
    entry = addEntry();
  }

  entry->setValue(rv);
}


void SendCoinsDialog::handleURI(const QString &uri)
{
  SendCoinsRecipient rv;
  if(!GUIUtil::parseBitcoinURI(uri, &rv))
  {
    return;
  }
  pasteEntry(rv);
}

void SendCoinsDialog::setBalance(qint64 balance, qint64 stake, qint64 unconfirmedBalance, qint64 reserveBalance)
{
  Q_UNUSED(stake);
  Q_UNUSED(unconfirmedBalance);
  Q_UNUSED(reserveBalance);
  if(!model || !model->getOptionsModel())
    return;

  int unit = model->getOptionsModel()->getDisplayUnit();
  ui->labelBalance->setText(BitcoinUnits::formatWithUnit(unit, balance));
}
