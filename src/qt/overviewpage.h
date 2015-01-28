#ifndef OVERVIEWPAGE_H
#define OVERVIEWPAGE_H

#include <QWidget>
#include <walletmodel.h>

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

namespace Ui {
  class OverviewPage;
}
class WalletModel;
class TxViewDelegate;

/** Overview ("home") page widget */
class OverviewPage : public QWidget
{
  Q_OBJECT

public:
  explicit OverviewPage(QWidget *parent = 0);
  ~OverviewPage();

  void setModel(WalletModel *model);
  
  public slots:
    void setBalance(qint64 balance, qint64 stake, qint64 unconfirmedBalance, 
                    BurnCoinsBalances burnBalances);
    void setNumTransactions(int count);

signals:
    void transactionClicked(const QModelIndex &index);

private:
    Ui::OverviewPage *ui;
    WalletModel *model;
    qint64 currentBalance;
    qint64 currentStake;
    qint64 currentUnconfirmedBalance;

    qint64 currentNetBurnCoins;
    qint64 currentEffectiveBurnCoins;
    qint64 currentImmatureBurnCoins;
    qint64 currentDecayedBurnCoins;


    TxViewDelegate *txdelegate;

    private slots:
      void displayUnitChanged();
};

#endif // OVERVIEWPAGE_H
